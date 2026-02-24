#include "LiangHyphenation.h"

#include <algorithm>
#include <vector>

/*
 * Liang hyphenation pipeline overview (Typst-style binary trie variant)
 * --------------------------------------------------------------------
 * 1.  Input normalization (buildAugmentedWord)
 *     - Accepts a vector of CodepointInfo structs emitted by the EPUB text
 *       parser. Each codepoint is validated with LiangWordConfig::isLetter so
 *       we abort early on digits, punctuation, etc. If the word is valid we
 *       build an "augmented" byte sequence: leading '.', lowercase UTF-8 bytes
 *       for every letter, then a trailing '.'. While doing this we capture the
 *       UTF-8 byte offset for each character and a reverse lookup table that
 *       maps UTF-8 byte indexes back to codepoint indexes. This lets the rest
 *       of the algorithm stay byte-oriented (matching the serialized automaton)
 *       while still emitting hyphen positions in codepoint space.
 *
 * 2.  Automaton decoding
 *     - SerializedHyphenationPatterns stores a contiguous blob generated from
 *       Typst's binary tries. The first 4 bytes contain the root offset. Each
 *       node packs transitions, variable-stride relative offsets to child
 *       nodes, and an optional pointer into a shared "levels" list. We parse
 *       that layout lazily via decodeState/transition, keeping everything in
 *       flash memory; no heap allocations besides the stack-local AutomatonState
 *       structs. getAutomaton caches parseAutomaton results per blob pointer so
 *       multiple words hitting the same language only pay the cost once.
 *
 * 3.  Pattern application
 *     - We walk the augmented bytes left-to-right. For each starting byte we
 *       stream transitions through the trie, terminating when a transition
 *       fails. Whenever a node exposes level data we expand the packed
 *       "dist+level" bytes: `dist` is the delta (in UTF-8 bytes) from the
 *       starting cursor and `level` is the Liang priority digit. Using the
 *       byte→codepoint lookup we mark the corresponding index in `scores`.
 *       Scores are only updated if the new level is higher, mirroring Liang's
 *       "max digit wins" rule.
 *
 * 4.  Output filtering
 *     - collectBreakIndexes converts odd-valued score entries back to codepoint
 *       break positions while enforcing `minPrefix`/`minSuffix` constraints from
 *       LiangWordConfig. The caller (language-specific hyphenators) can then
 *       translate these indexes into renderer glyph offsets, page layout data,
 *       etc.
 *
 * Keeping the entire algorithm small and deterministic is critical on the
 * ESP32-C3: we avoid recursion, dynamic allocations per node, or copying the
 * trie. All lookups stay within the generated blob, which lives in flash, and
 * the working buffers (augmented bytes/scores) scale with the word length rather
 * than the pattern corpus.
 *
 * Memory design note (heap fragmentation avoidance)
 * --------------------------------------------------
 * AugmentedWord previously held three std::vector<> members that were heap-
 * allocated and freed for every word during layout. For a German-language section
 * with hundreds of words, these thousands of small alloc/free cycles fragment
 * the heap enough to prevent large contiguous allocations (e.g. a 32 KB inflate
 * ring buffer) even when total free memory is sufficient.
 *
 * The fix replaces those vectors with fixed-size C arrays sized for the longest
 * plausible word. The longest known German word is ~63 codepoints; with up to
 * 2 UTF-8 bytes per German letter + 2 sentinel dots = 128 bytes. MAX_WORD_BYTES=160
 * and MAX_WORD_CHARS=70 give comfortable headroom. Words exceeding these limits
 * are silently skipped (no hyphenation), which is acceptable for correctness.
 * The struct lives on the render-task stack (8 KB) so no permanent DRAM is wasted.
 */

namespace {

using EmbeddedAutomaton = SerializedHyphenationPatterns;

// Upper bounds for the fixed word buffers. Sized for German (longest known word
// ≈63 codepoints × 2 UTF-8 bytes + 2 sentinel dots = 128 bytes). Words that
// exceed these limits are skipped rather than heap-allocated.
static constexpr size_t MAX_WORD_BYTES = 160;  // max UTF-8 bytes in augmented word
static constexpr size_t MAX_WORD_CHARS = 70;   // max codepoints + 2 sentinel dots

struct AugmentedWord {
  uint8_t bytes[MAX_WORD_BYTES];
  size_t charByteOffsets[MAX_WORD_CHARS];
  int32_t byteToCharIndex[MAX_WORD_BYTES];
  size_t byteLen = 0;
  size_t charCount_ = 0;

  bool empty() const { return byteLen == 0; }
  size_t charCount() const { return charCount_; }
};

// Encode a single Unicode codepoint into UTF-8 and append to word.bytes[].
// Returns the number of bytes written, or 0 if the codepoint is invalid or the
// buffer would overflow. Surrogates (0xD800–0xDFFF) and values above 0x10FFFF
// are not valid Unicode scalar values and are rejected.
size_t encodeUtf8(uint32_t cp, AugmentedWord& word) {
  if ((cp >= 0xD800u && cp <= 0xDFFFu) || cp > 0x10FFFFu) {
    return 0;
  }

  uint8_t encoded[4];
  size_t len = 0;

  if (cp <= 0x7Fu) {
    encoded[len++] = static_cast<uint8_t>(cp);
  } else if (cp <= 0x7FFu) {
    encoded[len++] = static_cast<uint8_t>(0xC0u | ((cp >> 6) & 0x1Fu));
    encoded[len++] = static_cast<uint8_t>(0x80u | (cp & 0x3Fu));
  } else if (cp <= 0xFFFFu) {
    encoded[len++] = static_cast<uint8_t>(0xE0u | ((cp >> 12) & 0x0Fu));
    encoded[len++] = static_cast<uint8_t>(0x80u | ((cp >> 6) & 0x3Fu));
    encoded[len++] = static_cast<uint8_t>(0x80u | (cp & 0x3Fu));
  } else {
    encoded[len++] = static_cast<uint8_t>(0xF0u | ((cp >> 18) & 0x07u));
    encoded[len++] = static_cast<uint8_t>(0x80u | ((cp >> 12) & 0x3Fu));
    encoded[len++] = static_cast<uint8_t>(0x80u | ((cp >> 6) & 0x3Fu));
    encoded[len++] = static_cast<uint8_t>(0x80u | (cp & 0x3Fu));
  }

  if (word.byteLen + len > MAX_WORD_BYTES) {
    return 0;  // overflow: word too long for fixed buffer, skip hyphenation
  }
  for (size_t i = 0; i < len; ++i) {
    word.bytes[word.byteLen++] = encoded[i];
  }
  return len;
}

// Build the dotted, lowercase UTF-8 representation plus lookup tables into `word`.
// Returns false if the word should be skipped (empty, non-letter, or too long).
bool buildAugmentedWord(AugmentedWord& word, const std::vector<CodepointInfo>& cps, const LiangWordConfig& config) {
  word.byteLen = 0;
  word.charCount_ = 0;

  if (cps.empty()) {
    return false;
  }

  // Leading sentinel '.'
  word.charByteOffsets[word.charCount_++] = 0;
  word.bytes[word.byteLen++] = '.';

  for (const auto& info : cps) {
    if (!config.isLetter(info.value)) {
      word.byteLen = 0;
      word.charCount_ = 0;
      return false;
    }
    // Reserve one slot for the trailing sentinel and check byte headroom.
    if (word.charCount_ >= MAX_WORD_CHARS - 1) {
      word.byteLen = 0;
      word.charCount_ = 0;
      return false;  // word too long
    }
    word.charByteOffsets[word.charCount_++] = word.byteLen;
    if (encodeUtf8(config.toLower(info.value), word) == 0) {
      word.byteLen = 0;
      word.charCount_ = 0;
      return false;  // byte buffer overflow
    }
  }

  // Trailing sentinel '.'
  if (word.charCount_ >= MAX_WORD_CHARS || word.byteLen >= MAX_WORD_BYTES) {
    word.byteLen = 0;
    word.charCount_ = 0;
    return false;
  }
  word.charByteOffsets[word.charCount_++] = word.byteLen;
  word.bytes[word.byteLen++] = '.';

  // Build byte→char reverse index: -1 for mid-codepoint bytes, char index for start bytes.
  for (size_t i = 0; i < word.byteLen; ++i) {
    word.byteToCharIndex[i] = -1;
  }
  for (size_t i = 0; i < word.charCount_; ++i) {
    const size_t offset = word.charByteOffsets[i];
    if (offset < word.byteLen) {
      word.byteToCharIndex[offset] = static_cast<int32_t>(i);
    }
  }

  return true;
}

// Decoded view of a single trie node pulled straight out of the serialized blob.
// - transitions: contiguous list of next-byte values
// - targets: packed relative offsets (1/2/3 bytes) for each transition
// - levels: optional pointer into the global levels list with packed dist/level pairs
struct AutomatonState {
  const uint8_t* data = nullptr;
  size_t size = 0;
  size_t addr = 0;
  uint8_t stride = 1;
  size_t childCount = 0;
  const uint8_t* transitions = nullptr;
  const uint8_t* targets = nullptr;
  const uint8_t* levels = nullptr;
  size_t levelsLen = 0;

  bool valid() const { return data != nullptr; }
};

// Interpret the node located at `addr`, returning transition metadata.
AutomatonState decodeState(const EmbeddedAutomaton& automaton, size_t addr) {
  AutomatonState state;
  if (addr >= automaton.size) {
    return state;
  }

  const uint8_t* base = automaton.data + addr;
  size_t remaining = automaton.size - addr;
  size_t pos = 0;

  const uint8_t header = base[pos++];
  // Header layout (bits):
  //   7        - hasLevels flag
  //   6..5     - stride selector (0 -> 1 byte, otherwise 1|2|3)
  //   4..0     - child count (5 bits), 31 == overflow -> extra byte
  const bool hasLevels = (header >> 7) != 0;
  uint8_t stride = static_cast<uint8_t>((header >> 5) & 0x03u);
  if (stride == 0) {
    stride = 1;
  }
  size_t childCount = static_cast<size_t>(header & 0x1Fu);
  if (childCount == 31u) {
    if (pos >= remaining) {
      return AutomatonState{};
    }
    childCount = base[pos++];
  }

  const uint8_t* levelsPtr = nullptr;
  size_t levelsLen = 0;
  if (hasLevels) {
    if (pos + 1 >= remaining) {
      return AutomatonState{};
    }
    const uint8_t offsetHi = base[pos++];
    const uint8_t offsetLoLen = base[pos++];
    // The 12-bit offset (hi<<4 | top nibble) points into the blob-level levels list.
    // The bottom nibble stores how many packed entries belong to this node.
    const size_t offset = (static_cast<size_t>(offsetHi) << 4) | (offsetLoLen >> 4);
    levelsLen = offsetLoLen & 0x0Fu;
    if (offset + levelsLen > automaton.size) {
      return AutomatonState{};
    }
    levelsPtr = automaton.data + offset - 4u;
  }

  if (pos + childCount > remaining) {
    return AutomatonState{};
  }
  const uint8_t* transitions = base + pos;
  pos += childCount;

  const size_t targetsBytes = childCount * stride;
  if (pos + targetsBytes > remaining) {
    return AutomatonState{};
  }
  const uint8_t* targets = base + pos;

  state.data = automaton.data;
  state.size = automaton.size;
  state.addr = addr;
  state.stride = stride;
  state.childCount = childCount;
  state.transitions = transitions;
  state.targets = targets;
  state.levels = levelsPtr;
  state.levelsLen = levelsLen;
  return state;
}

// Convert the packed stride-sized delta back into a signed offset.
int32_t decodeDelta(const uint8_t* buf, uint8_t stride) {
  if (stride == 1) {
    return static_cast<int8_t>(buf[0]);
  }
  if (stride == 2) {
    return static_cast<int16_t>((static_cast<uint16_t>(buf[0]) << 8) | static_cast<uint16_t>(buf[1]));
  }
  const int32_t unsignedVal =
      (static_cast<int32_t>(buf[0]) << 16) | (static_cast<int32_t>(buf[1]) << 8) | static_cast<int32_t>(buf[2]);
  return unsignedVal - (1 << 23);
}

// Follow a single byte transition from `state`, decoding the child node on success.
bool transition(const EmbeddedAutomaton& automaton, const AutomatonState& state, uint8_t letter, AutomatonState& out) {
  if (!state.valid()) {
    return false;
  }

  // Children remain sorted by letter in the serialized blob, but the lists are
  // short enough that a linear scan keeps code size down compared to binary search.
  for (size_t idx = 0; idx < state.childCount; ++idx) {
    if (state.transitions[idx] != letter) {
      continue;
    }
    const uint8_t* deltaPtr = state.targets + idx * state.stride;
    const int32_t delta = decodeDelta(deltaPtr, state.stride);
    // Deltas are relative to the current node's address, allowing us to keep all
    // targets within 24 bits while still referencing further nodes in the blob.
    const int64_t nextAddr = static_cast<int64_t>(state.addr) + delta;
    if (nextAddr < 0 || static_cast<size_t>(nextAddr) >= automaton.size) {
      return false;
    }
    out = decodeState(automaton, static_cast<size_t>(nextAddr));
    return out.valid();
  }
  return false;
}

// Converts odd score positions back into codepoint indexes, honoring min prefix/suffix constraints.
// Each break corresponds to scores[breakIndex + 1] because of the leading '.' sentinel.
// Convert odd score entries into hyphen positions while honoring prefix/suffix limits.
std::vector<size_t> collectBreakIndexes(const std::vector<CodepointInfo>& cps, const uint8_t* scores,
                                        const size_t scoresSize, const size_t minPrefix, const size_t minSuffix) {
  std::vector<size_t> indexes;
  const size_t cpCount = cps.size();
  if (cpCount < 2) {
    return indexes;
  }

  for (size_t breakIndex = 1; breakIndex < cpCount; ++breakIndex) {
    if (breakIndex < minPrefix) {
      continue;
    }

    const size_t suffixCount = cpCount - breakIndex;
    if (suffixCount < minSuffix) {
      continue;
    }

    const size_t scoreIdx = breakIndex + 1;
    if (scoreIdx >= scoresSize) {
      break;
    }
    if ((scores[scoreIdx] & 1u) == 0) {
      continue;
    }
    indexes.push_back(breakIndex);
  }

  return indexes;
}

}  // namespace

// Entry point that runs the full Liang pipeline for a single word.
std::vector<size_t> liangBreakIndexes(const std::vector<CodepointInfo>& cps,
                                      const SerializedHyphenationPatterns& patterns, const LiangWordConfig& config) {
  // AugmentedWord uses fixed-size C arrays (no heap allocation) to avoid
  // fragmenting the heap across hundreds of words during page layout.
  AugmentedWord augmented;
  if (!buildAugmentedWord(augmented, cps, config)) {
    return {};
  }

  const EmbeddedAutomaton& automaton = patterns;

  const AutomatonState root = decodeState(automaton, automaton.rootOffset);
  if (!root.valid()) {
    return {};
  }

  // Liang scores: one entry per augmented char (leading/trailing dots included).
  // Stack-allocated to avoid heap fragmentation (see memory design note above).
  uint8_t scores[MAX_WORD_CHARS];
  for (size_t i = 0; i < augmented.charCount_; ++i) {
    scores[i] = 0;
  }

  // Walk every starting character position and stream bytes through the trie.
  for (size_t charStart = 0; charStart < augmented.charCount_; ++charStart) {
    const size_t byteStart = augmented.charByteOffsets[charStart];
    AutomatonState state = root;

    for (size_t cursor = byteStart; cursor < augmented.byteLen; ++cursor) {
      AutomatonState next;
      if (!transition(automaton, state, augmented.bytes[cursor], next)) {
        break;  // No more matches for this prefix.
      }
      state = next;

      if (state.levels && state.levelsLen > 0) {
        size_t offset = 0;
        // Each packed byte stores the byte-distance delta and the Liang level digit.
        for (size_t i = 0; i < state.levelsLen; ++i) {
          const uint8_t packed = state.levels[i];
          const size_t dist = static_cast<size_t>(packed / 10);
          const uint8_t level = static_cast<uint8_t>(packed % 10);

          offset += dist;
          const size_t splitByte = byteStart + offset;
          if (splitByte >= augmented.byteLen) {
            continue;
          }

          const int32_t boundary = augmented.byteToCharIndex[splitByte];
          if (boundary < 0) {
            continue;  // Mid-codepoint byte, wait for the next one.
          }
          if (boundary < 2 || boundary + 2 > static_cast<int32_t>(augmented.charCount_)) {
            continue;  // Skip splits that land in the leading/trailing sentinels.
          }

          const size_t idx = static_cast<size_t>(boundary);
          if (idx >= augmented.charCount_) {
            continue;
          }
          scores[idx] = std::max(scores[idx], level);
        }
      }
    }
  }

  return collectBreakIndexes(cps, scores, augmented.charCount_, config.minPrefix, config.minSuffix);
}
