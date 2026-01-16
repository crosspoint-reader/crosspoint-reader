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
 */

namespace {

struct AugmentedWord {
  std::vector<uint8_t> bytes;
  std::vector<size_t> charByteOffsets;
  std::vector<int32_t> byteToCharIndex;

  bool empty() const { return bytes.empty(); }
  size_t charCount() const { return charByteOffsets.size(); }
};

// Encode a single Unicode codepoint into UTF-8 and append to the provided buffer.
size_t encodeUtf8(uint32_t cp, std::vector<uint8_t>& out) {
  if (cp <= 0x7Fu) {
    out.push_back(static_cast<uint8_t>(cp));
    return 1;
  }
  if (cp <= 0x7FFu) {
    out.push_back(static_cast<uint8_t>(0xC0u | ((cp >> 6) & 0x1Fu)));
    out.push_back(static_cast<uint8_t>(0x80u | (cp & 0x3Fu)));
    return 2;
  }
  if (cp <= 0xFFFFu) {
    out.push_back(static_cast<uint8_t>(0xE0u | ((cp >> 12) & 0x0Fu)));
    out.push_back(static_cast<uint8_t>(0x80u | ((cp >> 6) & 0x3Fu)));
    out.push_back(static_cast<uint8_t>(0x80u | (cp & 0x3Fu)));
    return 3;
  }
  out.push_back(static_cast<uint8_t>(0xF0u | ((cp >> 18) & 0x07u)));
  out.push_back(static_cast<uint8_t>(0x80u | ((cp >> 12) & 0x3Fu)));
  out.push_back(static_cast<uint8_t>(0x80u | ((cp >> 6) & 0x3Fu)));
  out.push_back(static_cast<uint8_t>(0x80u | (cp & 0x3Fu)));
  return 4;
}

// Build the dotted, lowercase UTF-8 representation plus lookup tables.
AugmentedWord buildAugmentedWord(const std::vector<CodepointInfo>& cps, const LiangWordConfig& config) {
  AugmentedWord word;
  if (cps.empty()) {
    return word;
  }

  word.bytes.reserve(cps.size() * 2 + 2);
  word.charByteOffsets.reserve(cps.size() + 2);

  word.charByteOffsets.push_back(0);
  word.bytes.push_back('.');

  for (const auto& info : cps) {
    if (!config.isLetter(info.value)) {
      word.bytes.clear();
      word.charByteOffsets.clear();
      word.byteToCharIndex.clear();
      return word;
    }
    word.charByteOffsets.push_back(word.bytes.size());
    encodeUtf8(config.toLower(info.value), word.bytes);
  }

  word.charByteOffsets.push_back(word.bytes.size());
  word.bytes.push_back('.');

  word.byteToCharIndex.assign(word.bytes.size(), -1);
  for (size_t i = 0; i < word.charByteOffsets.size(); ++i) {
    const size_t offset = word.charByteOffsets[i];
    if (offset < word.byteToCharIndex.size()) {
      word.byteToCharIndex[offset] = static_cast<int32_t>(i);
    }
  }
  return word;
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

// Lightweight descriptor for the entire embedded automaton.
// The blob format is:
//   [0..3]  - big-endian root offset
//   [4....] - node heap containing variable-sized headers + transition data
struct EmbeddedAutomaton {
  const uint8_t* data = nullptr;
  size_t size = 0;
  uint32_t rootOffset = 0;

  bool valid() const { return data != nullptr && size >= 4 && rootOffset < size; }
};

// Decode the serialized automaton header and root offset.
EmbeddedAutomaton parseAutomaton(const SerializedHyphenationPatterns& patterns) {
  EmbeddedAutomaton automaton;
  if (!patterns.data || patterns.size < 4) {
    return automaton;
  }

  automaton.data = patterns.data;
  automaton.size = patterns.size;
  automaton.rootOffset = (static_cast<uint32_t>(patterns.data[0]) << 24) |
                         (static_cast<uint32_t>(patterns.data[1]) << 16) |
                         (static_cast<uint32_t>(patterns.data[2]) << 8) | static_cast<uint32_t>(patterns.data[3]);
  if (automaton.rootOffset >= automaton.size) {
    automaton.data = nullptr;
    automaton.size = 0;
  }
  return automaton;
}

// Cache parsed automata per blob pointer to avoid reparsing.
const EmbeddedAutomaton& getAutomaton(const SerializedHyphenationPatterns& patterns) {
  struct CacheEntry {
    const SerializedHyphenationPatterns* key;
    EmbeddedAutomaton automaton;
  };
  static std::vector<CacheEntry> cache;

  for (const auto& entry : cache) {
    if (entry.key == &patterns) {
      return entry.automaton;
    }
  }

  cache.push_back({&patterns, parseAutomaton(patterns)});
  return cache.back().automaton;
}

// Interpret the node located at `addr`, returning transition metadata.
AutomatonState decodeState(const EmbeddedAutomaton& automaton, size_t addr) {
  AutomatonState state;
  if (!automaton.valid() || addr >= automaton.size) {
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
    levelsPtr = automaton.data + offset;
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
std::vector<size_t> collectBreakIndexes(const std::vector<CodepointInfo>& cps, const std::vector<uint8_t>& scores,
                                        const size_t minPrefix, const size_t minSuffix) {
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
    if (scoreIdx >= scores.size()) {
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
  const auto augmented = buildAugmentedWord(cps, config);
  if (augmented.empty()) {
    return {};
  }

  const EmbeddedAutomaton& automaton = getAutomaton(patterns);
  if (!automaton.valid()) {
    return {};
  }

  const AutomatonState root = decodeState(automaton, automaton.rootOffset);
  if (!root.valid()) {
    return {};
  }

  // Liang scores: one entry per augmented char (leading/trailing dots included).
  std::vector<uint8_t> scores(augmented.charCount(), 0);

  // Walk every starting character position and stream bytes through the trie.
  for (size_t charStart = 0; charStart < augmented.charByteOffsets.size(); ++charStart) {
    const size_t byteStart = augmented.charByteOffsets[charStart];
    AutomatonState state = root;

    for (size_t cursor = byteStart; cursor < augmented.bytes.size(); ++cursor) {
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
          if (splitByte >= augmented.byteToCharIndex.size()) {
            continue;
          }

          const int32_t boundary = augmented.byteToCharIndex[splitByte];
          if (boundary < 0) {
            continue;  // Mid-codepoint byte, wait for the next one.
          }
          if (boundary < 2 || boundary + 2 > static_cast<int32_t>(augmented.charCount())) {
            continue;  // Skip splits that land in the leading/trailing sentinels.
          }

          const size_t idx = static_cast<size_t>(boundary);
          if (idx >= scores.size()) {
            continue;
          }
          scores[idx] = std::max(scores[idx], level);
        }
      }
    }
  }

  return collectBreakIndexes(cps, scores, config.minPrefix, config.minSuffix);
}
