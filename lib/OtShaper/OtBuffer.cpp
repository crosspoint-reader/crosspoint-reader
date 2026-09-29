#include "OtBuffer.h"

#include <algorithm>
#include <cstring>

namespace ot {

namespace {
HeapCheck gHeapCheck = nullptr;
constexpr unsigned DIGEST_SHIFTS[3] = {4, 0, 6};  // hb_set_digest_shifts
}  // namespace

void setHeapCheck(const HeapCheck check) { gHeapCheck = check; }
bool heapAvailable(const size_t bytes) { return gHeapCheck == nullptr || gHeapCheck(bytes); }

void Digest::add(const uint32_t g) {
  for (int i = 0; i < 3; i++) masks[i] |= 1ull << ((g >> DIGEST_SHIFTS[i]) & 63);
}

void Digest::addRange(const uint32_t a, const uint32_t b) {
  if (masks[0] == ~0ull && masks[1] == ~0ull && masks[2] == ~0ull) return;
  for (int i = 0; i < 3; i++) {
    const unsigned shift = DIGEST_SHIFTS[i];
    if ((b >> shift) - (a >> shift) >= 63) {
      masks[i] = ~0ull;
    } else {
      const uint64_t ma = 1ull << ((a >> shift) & 63);
      const uint64_t mb = 1ull << ((b >> shift) & 63);
      masks[i] |= mb + (mb - ma) - (mb < ma ? 1 : 0);
    }
  }
}

bool Digest::mayHave(const uint32_t g) const {
  for (int i = 0; i < 3; i++) {
    if (!(masks[i] & (1ull << ((g >> DIGEST_SHIFTS[i]) & 63)))) return false;
  }
  return true;
}

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "digest records are little-endian in memory");

Digest readDigest(const uint8_t* record) {
  Digest d;
  memcpy(d.masks, record, sizeof(d.masks));
  return d;
}

void writeDigest(const Digest& digest, uint8_t* record) { memcpy(record, digest.masks, sizeof(digest.masks)); }

bool digestRecordMayHave(const uint8_t* record, const uint32_t glyph) {
  for (int i = 0; i < 3; i++) {
    uint64_t mask;
    memcpy(&mask, record + 8 * i, sizeof(mask));
    if (!(mask & (1ull << ((glyph >> DIGEST_SHIFTS[i]) & 63)))) return false;
  }
  return true;
}

bool Buffer::prepare(const unsigned maxGlyphs) {
  info.clear();
  pos.clear();
  scratch.clear();
  idx = 0;
  maxLength = maxGlyphs;
  successful = true;
  serial = 0;
  randomState = 1;
  hasDefaultIgnorables = hasBrokenSyllable = hasGposAttachment = false;
  digest = Digest();
  if (maxGlyphs > UINT16_MAX + 1u) return false;  // `indices` holds glyph positions as uint16_t
  if (info.capacity() >= maxGlyphs) return true;
  const size_t bytes =
      static_cast<size_t>(maxGlyphs) * (2 * sizeof(GlyphInfo) + sizeof(GlyphPosition) + sizeof(uint16_t));
  if (!heapAvailable(bytes)) return false;
  info.reserve(maxGlyphs);
  scratch.reserve(maxGlyphs);
  pos.reserve(maxGlyphs);
  indices.reserve(maxGlyphs);
  return true;
}

uint8_t Buffer::allocateLigId() {
  for (;;) {
    if (++serial == 0) ++serial;
    const auto id = static_cast<uint8_t>(serial & 0x07);
    if (id != 0) return id;
  }
}

bool Buffer::canGrow(const unsigned extra) {
  if (len() + extra <= maxLength) return true;
  successful = false;
  return false;
}

void Buffer::removeGlyph(const unsigned i) { info.erase(info.begin() + i); }

void Buffer::moveGlyph(const unsigned from, const unsigned to) {
  const GlyphInfo moved = info[from];
  for (unsigned i = from; i < to; i++) info[i] = info[i + 1];
  for (unsigned i = from; i > to; i--) info[i] = info[i - 1];
  info[to] = moved;
}

void Buffer::reverseRange(const unsigned start, const unsigned end) {
  for (unsigned i = start, j = end; i + 1 < j; i++, j--) std::swap(info[i], info[j - 1]);
}

void Buffer::refreshDigest() {
  digest = Digest();
  for (const GlyphInfo& g : info) digest.add(g.codepoint);
}

}  // namespace ot
