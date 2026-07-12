#pragma once
#include <cstdlib>
#include <cstdint>
inline uint32_t esp_random() { return (uint32_t)rand(); }
