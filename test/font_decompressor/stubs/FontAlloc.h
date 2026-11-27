#pragma once

#include <cstddef>

// FontDecompressorTest defines these to count and fail allocations.
extern "C" {
void* fiFontMalloc(size_t size);
void fiFontFree(void* ptr);
}
