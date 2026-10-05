#pragma once

#include <cstdint>

// Random 32-byte per-device secret, created on first use and kept in NVS: it
// survives SD swaps and reformats (only a full flash erase replaces it) and
// never leaves the device. Feeds plugin device IDs and the book-key wrap; each
// use derives its own value so none reveals another. False when NVS is
// unavailable: an unstable secret is worse than none.
bool deviceSecret(uint8_t (&out)[32]);
