#pragma once

#include <Arduino.h>
#include <mutex>

#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY 0xffffffffUL

using TaskHandle_t = void*;
using SemaphoreHandle_t = std::recursive_mutex*;

inline SemaphoreHandle_t xSemaphoreCreateMutex() { return new std::recursive_mutex(); }
inline int xSemaphoreTake(SemaphoreHandle_t sem, unsigned long) {
  sem->lock();
  return pdTRUE;
}
inline int xSemaphoreGive(SemaphoreHandle_t sem) {
  sem->unlock();
  return pdTRUE;
}
inline void* xSemaphoreGetMutexHolder(SemaphoreHandle_t) { return nullptr; }
inline int xQueuePeek(SemaphoreHandle_t, void*, int) { return pdTRUE; }

inline TaskHandle_t xTaskGetCurrentTaskHandle() { return nullptr; }
inline int xTaskCreate(void (*)(void*), const char*, uint32_t, void*, uint32_t, TaskHandle_t* handle) {
  if (handle) *handle = reinterpret_cast<TaskHandle_t>(1);
  return pdTRUE;
}
enum eNotifyAction { eIncrement };
inline int xTaskNotify(TaskHandle_t, uint32_t, eNotifyAction) { return pdTRUE; }
inline uint32_t ulTaskNotifyTake(int, unsigned long) { return 1; }
inline void vTaskDelay(uint32_t ms) { delay(ms); }
inline void taskENTER_CRITICAL(void*) {}
inline void taskEXIT_CRITICAL(void*) {}
