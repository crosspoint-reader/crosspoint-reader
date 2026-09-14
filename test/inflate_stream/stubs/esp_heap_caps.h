#pragma once
#include <cstddef>
#include <cstdint>
constexpr uint32_t MALLOC_CAP_DEFAULT = 1;
constexpr uint32_t MALLOC_CAP_SPIRAM = 2;
constexpr uint32_t MALLOC_CAP_INTERNAL = 4;
inline size_t heap_caps_get_free_size(uint32_t) { return 0; }
inline size_t heap_caps_get_largest_free_block(uint32_t) { return 0; }
inline size_t heap_caps_get_total_size(uint32_t) { return 0; }
inline size_t heap_caps_get_minimum_free_size(uint32_t) { return 0; }
