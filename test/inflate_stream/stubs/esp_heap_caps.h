#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
constexpr uint32_t MALLOC_CAP_DEFAULT = 1;
constexpr uint32_t MALLOC_CAP_SPIRAM = 2;
constexpr uint32_t MALLOC_CAP_INTERNAL = 4;
constexpr uint32_t MALLOC_CAP_8BIT = 8;
inline void* heap_caps_malloc(size_t size, uint32_t) { return std::malloc(size); }
inline void heap_caps_free(void* ptr) { std::free(ptr); }
inline size_t heap_caps_get_free_size(uint32_t) { return 0; }
inline size_t heap_caps_get_largest_free_block(uint32_t) { return 0; }
inline size_t heap_caps_get_total_size(uint32_t) { return 0; }
inline size_t heap_caps_get_minimum_free_size(uint32_t) { return 0; }
