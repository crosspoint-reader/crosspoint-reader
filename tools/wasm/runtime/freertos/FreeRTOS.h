#pragma once

#include <cstdint>

#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define eIncrement 1
#define portTICK_PERIOD_MS 1
#define pdMS_TO_TICKS(ms) (static_cast<uint32_t>(ms) / portTICK_PERIOD_MS)

using BaseType_t = int;
using TickType_t = uint32_t;
struct BrowserTask;
using TaskHandle_t = BrowserTask*;

// Cooperative tasks cannot preempt a critical section; yielding inside one is invalid.
struct portMUX_TYPE {};
#define portMUX_INITIALIZER_UNLOCKED \
  {                                  \
  }
void taskENTER_CRITICAL(portMUX_TYPE*);
void taskEXIT_CRITICAL(portMUX_TYPE*);
#define portENTER_CRITICAL(mux) taskENTER_CRITICAL(mux)
#define portEXIT_CRITICAL(mux) taskEXIT_CRITICAL(mux)
