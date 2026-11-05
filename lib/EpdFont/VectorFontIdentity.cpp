#include "VectorFontSupport.h"

#if CROSSPOINT_VECTOR_FONTS
#include "VectorFontIdentity.h"

#include <HalStorage.h>

#include <algorithm>

namespace {
uint32_t be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
void addByte(uint32_t& hash, const uint8_t byte) {
  hash = (hash ^ byte) * 16777619u;
}
bool readAt(HalFile& file, const uint32_t offset, void* out, const size_t count) {
  return uint64_t(offset) + count <= file.size() && file.seek(offset) &&
         file.read(out, count) == static_cast<int>(count);
}
bool nameTableHash(HalFile& file, uint32_t& hash) {
  uint8_t header[16];
  if (!readAt(file, 0, header, 12)) return false;
  uint32_t faceOffset = 0;
  if (be32(header) == 0x74746366u) {  // ttcf: match FreeType's face index 0.
    if (be32(header + 8) == 0 || !readAt(file, 12, header, 4)) return false;
    faceOffset = be32(header);
    if (!readAt(file, faceOffset, header, 12)) return false;
  }
  const uint32_t signature = be32(header);
  if (signature != 0x00010000u && signature != 0x4F54544Fu && signature != 0x74727565u) return false;
  const uint16_t count = (uint16_t(header[4]) << 8) | header[5];
  const uint64_t directoryEnd = uint64_t(faceOffset) + 12 + uint64_t(count) * 16;
  if (directoryEnd > file.size() || directoryEnd > 0x100000000ull) return false;
  for (uint16_t i = 0; i < count; ++i) {
    if (!readAt(file, faceOffset + 12 + uint32_t(i) * 16, header, 16)) return false;
    if (be32(header) != 0x6E616D65u) continue;  // name
    uint32_t offset = be32(header + 8);
    uint32_t remaining = be32(header + 12);
    const uint64_t end = uint64_t(offset) + remaining;
    if (remaining == 0 || end > file.size() || end > 0x100000000ull) return false;
    uint8_t chunk[64];
    while (remaining) {
      const size_t n = std::min<size_t>(remaining, sizeof(chunk));
      if (!readAt(file, offset, chunk, n)) return false;
      for (size_t j = 0; j < n; ++j) addByte(hash, chunk[j]);
      remaining -= n;
      if (remaining) offset += n;
    }
    return true;
  }
  return false;
}
}  // namespace

uint32_t vectorFontSourceHash(HalFile& file, const char* filename) {
  const auto position = file.position();
  uint32_t hash = 2166136261u;
  addByte(hash, 1);  // Separate table hashes from filename fallbacks.
  const bool found = nameTableHash(file, hash);
  file.seek(position);
  if (found) return hash;
  hash = 2166136261u;
  addByte(hash, 0);
  for (const char* p = filename; p && *p; ++p) addByte(hash, static_cast<uint8_t>(*p));
  return hash;
}

int vectorFontIdentity(const uint32_t sourceHashes[4], const uint8_t presentMask, const uint8_t pointSize,
                       const bool uiFallback) {
  uint32_t hash = 2166136261u;
  // "TTF-name-v1" prefix, so we can "invalidate" existing hashes after a release if
  // font handling changes fundamentally (again).
  for (const char c : "TTF-name-v1") addByte(hash, c);
  addByte(hash, pointSize);
  addByte(hash, uiFallback);
  addByte(hash, presentMask);
  for (uint8_t role = 0; role < 4; ++role) {
    if (!(presentMask & (1u << role))) continue;
    for (int shift = 24; shift >= 0; shift -= 8) addByte(hash, static_cast<uint8_t>(sourceHashes[role] >> shift));
  }
  const int id = static_cast<int>(hash);
  return id != 0 ? id : 1;
}
#endif
