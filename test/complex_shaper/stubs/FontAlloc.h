#pragma once

#include <cstdlib>

extern "C" {
inline void* fiFontMalloc(size_t size) { return std::malloc(size); }
inline void* fiFontRealloc(void* ptr, size_t size) { return std::realloc(ptr, size); }
inline void fiFontFree(void* ptr) { std::free(ptr); }
}
