#include "ThaiDict.h"

#include <cstring>

#include "ThaiDictData.h"

namespace {

// Decode a block sequentially, comparing each word against target[0..len).
// Returns true on an exact match. Words are sorted, so decoding stops early
// once the current word exceeds the target.
bool blockContains(const uint32_t blockIndex, const uint8_t* target, const size_t len) {
  using namespace thaidict;
  const uint8_t* p = DATA + BLOCK_OFFSETS[blockIndex];
  const uint32_t wordsInBlock = (blockIndex == BLOCK_COUNT - 1) ? (WORD_COUNT - blockIndex * BLOCK_SIZE) : BLOCK_SIZE;

  uint8_t word[MAX_WORD_LEN];
  uint8_t wordLen = *p++;
  memcpy(word, p, wordLen);
  p += wordLen;

  for (uint32_t i = 0;; i++) {
    const int cmp = memcmp(word, target, wordLen < len ? wordLen : len);
    if (cmp == 0 && wordLen == len) return true;
    if (cmp > 0 || (cmp == 0 && wordLen > len)) return false;  // sorted: past the target

    if (i + 1 >= wordsInBlock) return false;
    const uint8_t prefixLen = *p++;
    const uint8_t suffixLen = *p++;
    memcpy(word + prefixLen, p, suffixLen);
    p += suffixLen;
    wordLen = prefixLen + suffixLen;
  }
}

// Compare target[0..len) against block blockIndex's first (fully spelled) word.
int compareBlockHead(const uint32_t blockIndex, const uint8_t* target, const size_t len) {
  using namespace thaidict;
  const uint8_t* p = DATA + BLOCK_OFFSETS[blockIndex];
  const uint8_t headLen = *p++;
  const int cmp = memcmp(p, target, headLen < len ? headLen : len);
  if (cmp != 0) return cmp;
  return static_cast<int>(headLen) - static_cast<int>(len);
}

// Exact-match lookup of target[0..len) over the front-coded sorted list:
// binary search for the last block whose head is <= target, then decode it.
bool contains(const uint8_t* target, const size_t len) {
  using namespace thaidict;
  uint32_t lo = 0, hi = BLOCK_COUNT;  // invariant: block[lo].head <= target < block[hi].head
  if (compareBlockHead(0, target, len) > 0) return false;
  while (hi - lo > 1) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (compareBlockHead(mid, target, len) <= 0) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return blockContains(lo, target, len);
}

}  // namespace

namespace ThaiDict {

size_t matchLongest(const uint32_t* cps, const size_t count) {
  using namespace thaidict;

  // Byte-map the window once; stop at the first non-Thai codepoint.
  uint8_t mapped[MAX_WORD_LEN];
  size_t mappedLen = 0;
  const size_t window = count < MAX_WORD_LEN ? count : MAX_WORD_LEN;
  while (mappedLen < window) {
    const uint32_t cp = cps[mappedLen];
    if (cp < 0x0E01 || cp > 0x0E5B) break;
    mapped[mappedLen++] = static_cast<uint8_t>(cp - 0x0E00);
  }

  for (size_t len = mappedLen; len > 0; len--) {
    if (contains(mapped, len)) return len;
  }
  return 0;
}

}  // namespace ThaiDict
