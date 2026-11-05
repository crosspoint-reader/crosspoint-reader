#pragma once

#include <cstdint>

class HalFile;

// Hash first face's raw name table, fallback to the filename when not available.
uint32_t vectorFontSourceHash(HalFile& file, const char* filename);
int vectorFontIdentity(const uint32_t sourceHashes[4], uint8_t presentMask, uint8_t pointSize, bool uiFallback = false);
