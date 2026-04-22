#include "MalayalamShaper.h"

#include <algorithm>
#include <cstring>

#include "MalayalamShapingData.h"

using namespace MalayalamShapingData;

// Maximum codepoints in a single word we'll process
static constexpr size_t MAX_WORD_CPS = 128;

static inline bool isMalayalam(uint32_t cp) {
  return (cp >= 0x0D00 && cp <= 0x0D7F) || (cp >= PUA_START && cp <= PUA_END);
}

static inline bool isMalayalamConsonant(uint32_t cp) { return cp >= 0x0D15 && cp <= 0x0D39; }

static inline bool isVirama(uint32_t cp) { return cp == 0x0D4D; }

// Pre-base (left-side) matras that must be visually reordered before the base consonant.
static inline bool isPreBaseMatra(uint32_t cp) {
  return cp == 0x0D46    // െ  VOWEL SIGN E
         || cp == 0x0D47 // േ  VOWEL SIGN EE
         || cp == 0x0D48 // ൈ  VOWEL SIGN AI
      ;
}

// Decompose composite vowel signs into left + right parts.
// Returns true if decomposed, writing to out1 (pre-base) and out2 (post-base).
static bool decomposeComposite(uint32_t cp, uint32_t& out1, uint32_t& out2) {
  switch (cp) {
    case 0x0D4A:  // ൊ  -> െ + ാ
      out1 = 0x0D46;
      out2 = 0x0D3E;
      return true;
    case 0x0D4B:  // ോ  -> േ + ാ
      out1 = 0x0D47;
      out2 = 0x0D3E;
      return true;
    case 0x0D4C:  // ൌ  -> െ + ൗ
      out1 = 0x0D46;
      out2 = 0x0D57;
      return true;
    default:
      return false;
  }
}

// Decompose composite matras in-place, expanding the codepoint array.
static void decomposeMatras(uint32_t* cps, size_t& count) {
  uint32_t tmp[MAX_WORD_CPS];
  size_t tmpCount = 0;

  for (size_t i = 0; i < count && tmpCount < MAX_WORD_CPS - 1; i++) {
    uint32_t left, right;
    if (decomposeComposite(cps[i], left, right)) {
      tmp[tmpCount++] = left;
      if (tmpCount < MAX_WORD_CPS) tmp[tmpCount++] = right;
    } else {
      tmp[tmpCount++] = cps[i];
    }
  }

  memcpy(cps, tmp, tmpCount * sizeof(uint32_t));
  count = tmpCount;
}

// Reorder pre-base matras to appear before their base consonant cluster.
// In Unicode order: consonant [+ virama + consonant]* + matra
// After reorder:    matra + consonant [+ virama + consonant]*
// Works in-place to avoid value-based index searching (which breaks when
// a cluster contains duplicate consonants, e.g. ച്ചെ).
static void reorderPreBaseMatras(uint32_t* cps, size_t& count) {
  for (size_t i = 1; i < count; i++) {
    if (!isPreBaseMatra(cps[i])) continue;

    // The consonant immediately before the matra is the last in the cluster.
    size_t lastCons = i - 1;
    if (!isMalayalamConsonant(cps[lastCons])) continue;

    // Walk backwards past (virama + consonant) pairs to find the cluster start.
    size_t clusterStart = lastCons;
    while (clusterStart >= 2 && isVirama(cps[clusterStart - 1]) && isMalayalamConsonant(cps[clusterStart - 2])) {
      clusterStart -= 2;
    }

    // Move the matra from position i to position clusterStart.
    // Shift cps[clusterStart .. i-1] right by 1 to make room.
    uint32_t matra = cps[i];
    memmove(&cps[clusterStart + 1], &cps[clusterStart], (i - clusterStart) * sizeof(uint32_t));
    cps[clusterStart] = matra;
    // i stays the same — the matra moved behind us, nothing to skip.
  }
}

uint32_t MalayalamShaper::nextCodepoint(const char*& p, const char* end) {
  if (p >= end) return 0;
  uint8_t b = static_cast<uint8_t>(*p);
  uint32_t cp;
  int len;

  if (b < 0x80) {
    cp = b;
    len = 1;
  } else if ((b & 0xE0) == 0xC0) {
    cp = b & 0x1F;
    len = 2;
  } else if ((b & 0xF0) == 0xE0) {
    cp = b & 0x0F;
    len = 3;
  } else if ((b & 0xF8) == 0xF0) {
    cp = b & 0x07;
    len = 4;
  } else {
    p++;
    return 0xFFFD;
  }

  if (p + len > end) {
    p = end;
    return 0xFFFD;
  }

  for (int i = 1; i < len; i++) {
    uint8_t cb = static_cast<uint8_t>(p[i]);
    if ((cb & 0xC0) != 0x80) {
      p++;
      return 0xFFFD;
    }
    cp = (cp << 6) | (cb & 0x3F);
  }
  p += len;
  return cp;
}

size_t MalayalamShaper::encodeCodepoint(uint32_t cp, char*& out, const char* outEnd) {
  if (cp < 0x80) {
    if (out >= outEnd) return 0;
    *out++ = static_cast<char>(cp);
    return 1;
  } else if (cp < 0x800) {
    if (out + 2 > outEnd) return 0;
    *out++ = static_cast<char>(0xC0 | (cp >> 6));
    *out++ = static_cast<char>(0x80 | (cp & 0x3F));
    return 2;
  } else if (cp < 0x10000) {
    if (out + 3 > outEnd) return 0;
    *out++ = static_cast<char>(0xE0 | (cp >> 12));
    *out++ = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    *out++ = static_cast<char>(0x80 | (cp & 0x3F));
    return 3;
  } else {
    if (out + 4 > outEnd) return 0;
    *out++ = static_cast<char>(0xF0 | (cp >> 18));
    *out++ = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    *out++ = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    *out++ = static_cast<char>(0x80 | (cp & 0x3F));
    return 4;
  }
}

// Binary search for Rule2 by (in0, in1)
static uint32_t matchRule2(uint32_t in0, uint32_t in1) {
  int lo = 0, hi = static_cast<int>(rules2Count) - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    const auto& r = rules2[mid];
    if (r.in0 < in0 || (r.in0 == in0 && r.in1 < in1)) {
      lo = mid + 1;
    } else if (r.in0 > in0 || (r.in0 == in0 && r.in1 > in1)) {
      hi = mid - 1;
    } else {
      return r.out;
    }
  }
  return 0;
}

// Binary search for Rule3 by (in0, in1, in2)
static uint32_t matchRule3(uint32_t in0, uint32_t in1, uint32_t in2) {
  int lo = 0, hi = static_cast<int>(rules3Count) - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    const auto& r = rules3[mid];
    if (r.in0 < in0 || (r.in0 == in0 && r.in1 < in1) ||
        (r.in0 == in0 && r.in1 == in1 && r.in2 < in2)) {
      lo = mid + 1;
    } else if (r.in0 > in0 || (r.in0 == in0 && r.in1 > in1) ||
               (r.in0 == in0 && r.in1 == in1 && r.in2 > in2)) {
      hi = mid - 1;
    } else {
      return r.out;
    }
  }
  return 0;
}

// Linear scan for Rule5 (only 1 entry, not worth binary search)
static uint32_t matchRule5(uint32_t in0, uint32_t in1, uint32_t in2, uint32_t in3, uint32_t in4) {
  for (uint16_t i = 0; i < rules5Count; i++) {
    const auto& r = rules5[i];
    if (r.in0 == in0 && r.in1 == in1 && r.in2 == in2 && r.in3 == in3 && r.in4 == in4) {
      return r.out;
    }
  }
  return 0;
}

uint32_t MalayalamShaper::tryMatch(const uint32_t* cps, size_t count, size_t& pos) {
  size_t remaining = count - pos;

  // Try longest match first (5-char)
  if (remaining >= 5) {
    uint32_t out = matchRule5(cps[pos], cps[pos + 1], cps[pos + 2], cps[pos + 3], cps[pos + 4]);
    if (out) {
      pos += 5;
      return out;
    }
  }

  // Try 3-char match
  if (remaining >= 3) {
    uint32_t out = matchRule3(cps[pos], cps[pos + 1], cps[pos + 2]);
    if (out) {
      pos += 3;
      return out;
    }
  }

  // Try 2-char match
  if (remaining >= 2) {
    uint32_t out = matchRule2(cps[pos], cps[pos + 1]);
    if (out) {
      pos += 2;
      return out;
    }
  }

  return 0;
}

size_t MalayalamShaper::shape(const char* input, size_t inputLen, char* output, size_t outputCap) {
  const char* p = input;
  const char* end = input + inputLen;
  char* out = output;
  const char* outEnd = output + outputCap;

  // Decode all codepoints into a flat array for lookahead
  uint32_t cps[MAX_WORD_CPS];
  size_t cpCount = 0;

  while (p < end && cpCount < MAX_WORD_CPS) {
    cps[cpCount++] = nextCodepoint(p, end);
  }

  // Pre-processing: decompose composite matras and reorder pre-base matras
  // before the base consonant cluster. This matches the Unicode Indic shaping
  // model where reordering happens before GSUB feature application.
  decomposeMatras(cps, cpCount);
  reorderPreBaseMatras(cps, cpCount);

  // Phase 1: Form conjuncts RIGHT-TO-LEFT within consonant clusters.
  //
  // In Indic shaping the "base" consonant is the rightmost in a cluster, so
  // conjuncts must form from the right.  Example: സ്ക്ക (sa+virama+ka+virama+ka)
  //   Right-to-left: ka+virama+ka → kka first, then sa+virama+kka → skka.
  //   Left-to-right would wrongly grab sa+virama+ka → ska, orphaning virama+ka.
  //
  // We repeatedly scan right-to-left for the rightmost 3-char C+virama+C match
  // (or PUA+virama+C for triple+ stacking) and replace it, until no more form.
  {
    bool changed = true;
    while (changed) {
      changed = false;
      for (int i = static_cast<int>(cpCount) - 3; i >= 0; i--) {
        if (!isVirama(cps[i + 1])) continue;
        if (!isMalayalam(cps[i]) || !isMalayalam(cps[i + 2])) continue;
        uint32_t out = matchRule3(cps[i], cps[i + 1], cps[i + 2]);
        if (out) {
          cps[i] = out;
          memmove(&cps[i + 1], &cps[i + 3], (cpCount - i - 3) * sizeof(uint32_t));
          cpCount -= 2;
          changed = true;
          break;  // restart scan from right after each replacement
        }
      }
    }
  }

  // Phase 2: Left-to-right rule application for remaining substitutions
  // (vowel signs, PUA+vowel chaining, chillu forms, etc.).
  // Multi-pass needed because some rules chain (e.g., conjunct + vowel sign).
  {
    bool changed = true;
    while (changed) {
      changed = false;
      uint32_t newCps[MAX_WORD_CPS];
      size_t newCount = 0;
      size_t pos = 0;

      while (pos < cpCount) {
        if (isMalayalam(cps[pos])) {
          uint32_t out_cp = tryMatch(cps, cpCount, pos);
          if (out_cp) {
            newCps[newCount++] = out_cp;
            changed = true;
            continue;
          }
        }
        newCps[newCount++] = cps[pos++];
      }

      if (changed) {
        memcpy(cps, newCps, newCount * sizeof(uint32_t));
        cpCount = newCount;
      }
    }
  }

  // Encode shaped codepoints back to UTF-8
  for (size_t i = 0; i < cpCount; i++) {
    if (encodeCodepoint(cps[i], out, outEnd) == 0) break;
  }

  return static_cast<size_t>(out - output);
}

bool MalayalamShaper::containsMalayalam(const char* text, size_t len) {
  const char* p = text;
  const char* end = text + len;
  while (p < end) {
    uint32_t cp = nextCodepoint(p, end);
    if ((cp >= 0x0D00 && cp <= 0x0D7F) || (cp >= PUA_START && cp <= PUA_END)) return true;
  }
  return false;
}
