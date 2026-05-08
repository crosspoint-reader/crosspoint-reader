#pragma once
#include "Arduino.h"

inline uint32_t esp_get_free_heap_size() { return ESP.getFreeHeap(); }
