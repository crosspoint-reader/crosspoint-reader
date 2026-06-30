#pragma once

#include <esp_err.h>
#include <esp_task_wdt.h>

inline void resetTaskWatchdogIfSubscribed() {
  if (esp_task_wdt_status(nullptr) == ESP_OK) {
    esp_task_wdt_reset();
  }
}
