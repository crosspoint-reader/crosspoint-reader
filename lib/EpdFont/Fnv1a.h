#pragma once

#include <cstddef>
#include <cstdint>

// 32-bit FNV-1a. The shaping blob's content key is also computed by
// lib/EpdFont/scripts/shaping_blob.py, so the constants must not change.
namespace fnv1a {

constexpr uint32_t OFFSET_BASIS = 2166136261u;
constexpr uint32_t PRIME = 16777619u;

// Hashes `length` bytes; pass a previous result as `seed` to continue it.
inline uint32_t hash(const void* data, const size_t length, const uint32_t seed = OFFSET_BASIS) {
  uint32_t h = seed;
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < length; i++) {
    h ^= bytes[i];
    h *= PRIME;
  }
  return h;
}

}  // namespace fnv1a
