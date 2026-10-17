#pragma once

#include "FreeRTOS.h"

BaseType_t xTaskCreate(void (*fn)(void*), const char* name, uint32_t stackDepth, void* param, BaseType_t priority,
                       TaskHandle_t* handle);
inline BaseType_t xTaskCreatePinnedToCore(void (*fn)(void*), const char* name, uint32_t stackDepth, void* param,
                                          BaseType_t priority, TaskHandle_t* handle, BaseType_t) {
  return xTaskCreate(fn, name, stackDepth, param, priority, handle);
}
TaskHandle_t xTaskGetCurrentTaskHandle();
uint32_t ulTaskNotifyTake(int clearOnExit, uint32_t ticksToWait);
void xTaskNotify(TaskHandle_t handle, uint32_t value, int action);
const char* pcTaskGetName(TaskHandle_t handle);
void vTaskDelete(TaskHandle_t handle);
void vTaskDelay(uint32_t ticks);
inline unsigned int uxTaskGetStackHighWaterMark(TaskHandle_t) { return 2048; }
inline void vTaskList(char*) {}

namespace browser {
// Run ready fibers, then return the delay before the browser should call again.
int stepTasks();
void stopTasks();
}  // namespace browser
