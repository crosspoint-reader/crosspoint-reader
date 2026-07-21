#pragma once
#include <cstdint>
inline void esp_restart() {}
inline uint32_t esp_get_free_heap_size() { return 1000000; }