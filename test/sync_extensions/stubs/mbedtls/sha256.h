#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

// Not SHA-256: the merge logic only needs a stable id per XPath.
inline int mbedtls_sha256(const unsigned char* input, size_t length, unsigned char* output, int) {
  uint64_t hash = 1469598103934665603ull;
  for (size_t i = 0; i < length; ++i) hash = (hash ^ input[i]) * 1099511628211ull;
  memset(output, 0, 32);
  memcpy(output, &hash, sizeof(hash));
  return 0;
}
