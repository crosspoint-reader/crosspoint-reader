#include "VectorFontSupport.h"

#if CROSSPOINT_VECTOR_FONTS
#include "VectorFontIdentity.h"

namespace {
void addByte(uint32_t& hash, const uint8_t byte) { hash = (hash ^ byte) * 16777619u; }
}  // namespace

int vectorFontIdentity(const char* familyName, const uint32_t contentKey, const uint8_t pointSize,
                       const bool uiFallback) {
  uint32_t hash = 2166136261u;
  // Versioned prefix for cache invalidation when font identity rules change.
  for (const char c : "TTF-content-v1") addByte(hash, c);
  for (const char* p = familyName; p && *p; ++p) addByte(hash, static_cast<uint8_t>(*p));
  addByte(hash, 0);
  for (int shift = 24; shift >= 0; shift -= 8) addByte(hash, static_cast<uint8_t>(contentKey >> shift));
  addByte(hash, pointSize);
  addByte(hash, uiFallback);
  const int id = static_cast<int>(hash);
  return id != 0 ? id : 1;
}
#endif
