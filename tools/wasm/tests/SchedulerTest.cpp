#include <emscripten/emscripten.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <cassert>
#include <cstdarg>
#include <cstdio>

void logPrintf(const char*, const char*, const char* format, ...) {
  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
}

namespace {
TaskHandle_t waiter;
SemaphoreHandle_t mutex;
SemaphoreHandle_t binary;
SemaphoreHandle_t recursive;
bool holding = false;
bool waiting = false;
bool completed = false;
bool holderCompleted = false;
int executions = 0;

void waitTask(void*) {
  assert(xTaskGetCurrentTaskHandle() == waiter);
  assert(ulTaskNotifyTake(pdTRUE, 0) == 0);
  waiting = true;
  // The producer sends two notifications before yielding; neither can be lost.
  assert(ulTaskNotifyTake(pdFALSE, portMAX_DELAY) == 2);
  assert(ulTaskNotifyTake(pdTRUE, 0) == 1);
  assert(ulTaskNotifyTake(pdTRUE, 0) == 0);
  const double started = emscripten_get_now();
  assert(ulTaskNotifyTake(pdTRUE, 5) == 0);
  assert(emscripten_get_now() - started >= 5);
  assert(holding);
  assert(!xSemaphoreTake(mutex, 0));
  assert(xSemaphoreGetMutexHolder(mutex) != waiter);
  assert(xQueuePeek(mutex, nullptr, 0) == pdFALSE);
  assert(!xSemaphoreGive(mutex));
  assert(!xSemaphoreTake(mutex, 2));
  assert(xSemaphoreTake(mutex, portMAX_DELAY));
  assert(xSemaphoreGetMutexHolder(mutex) == waiter);
  assert(!xSemaphoreTake(mutex, 0));
  assert(xSemaphoreGive(mutex));
  assert(xSemaphoreGetMutexHolder(mutex) == nullptr);
  assert(!xSemaphoreTake(binary, 2));
  assert(xSemaphoreTake(binary, portMAX_DELAY));
  assert(!xSemaphoreTake(binary, 0));
  assert(xSemaphoreTakeRecursive(recursive, 0));
  assert(xSemaphoreTakeRecursive(recursive, 0));
  assert(xSemaphoreGiveRecursive(recursive));
  assert(xSemaphoreGetMutexHolder(recursive) == waiter);
  assert(xSemaphoreGiveRecursive(recursive));
  assert(xSemaphoreGetMutexHolder(recursive) == nullptr);
  completed = true;
}

void holderTask(void*) {
  while (!waiting) vTaskDelay(0);
  assert(xSemaphoreTake(mutex, 0));
  holding = true;
  xTaskNotify(waiter, 1, eIncrement);
  xTaskNotify(waiter, 1, eIncrement);
  vTaskDelay(20);
  assert(xSemaphoreGive(mutex));
  holding = false;
  vTaskDelay(10);
  assert(xSemaphoreGive(binary));
  assert(!xSemaphoreGive(binary));
  holderCompleted = true;
}

void countTask(void*) { ++executions; }
void deletedTask(void*) {
  vTaskDelete(nullptr);
  assert(false && "Deleted task resumed");
}
void delayedTask(void*) {
  vTaskDelay(50);
  ++executions;
}
void pumpUntil(bool* done) {
  const double timeout = emscripten_get_now() + 1000;
  while (!*done && emscripten_get_now() < timeout) browser::stepTasks();
  assert(*done);
}
}  // namespace

int main() {
  mutex = xSemaphoreCreateMutex();
  binary = xSemaphoreCreateBinary();
  recursive = xSemaphoreCreateRecursiveMutex();
  assert(mutex && binary && recursive);
  assert(!xSemaphoreTake(binary, 0));
  assert(xTaskCreate(waitTask, "waiter", 8192, nullptr, 1, &waiter));
  assert(xTaskCreate(holderTask, "holder", 8192, nullptr, 1, nullptr));
  pumpUntil(&completed);
  assert(holderCompleted);
  // Natural return and self-deletion release task slots for repeated use.
  for (int i = 0; i < 12; ++i) {
    assert(xTaskCreate(deletedTask, "deleted", 8192, nullptr, 1, nullptr));
    assert(xTaskCreate(countTask, "counter", 8192, nullptr, 1, nullptr));
    browser::stepTasks();
    assert(executions == i + 1);
  }
  TaskHandle_t delayed = nullptr;
  assert(xTaskCreate(delayedTask, "delayed", 8192, nullptr, 1, &delayed));
  browser::stepTasks();
  vTaskDelete(delayed);
  for (int i = 0; i < 4; ++i) assert(xTaskCreate(countTask, "capacity", 8192, nullptr, 1, nullptr));
  assert(!xTaskCreate(countTask, "overflow", 8192, nullptr, 1, &delayed));
  assert(!delayed);
  browser::stepTasks();
  assert(executions == 16);
  vSemaphoreDelete(mutex);
  vSemaphoreDelete(binary);
  vSemaphoreDelete(recursive);
  static SemaphoreHandle_t handles[32];
  for (auto& handle : handles) {
    handle = xSemaphoreCreateMutex();
    assert(handle);
  }
  assert(!xSemaphoreCreateMutex());
  for (auto handle : handles) vSemaphoreDelete(handle);
  assert(xSemaphoreCreateMutex());
  assert(xTaskCreate(countTask, "stopped", 8192, nullptr, 1, nullptr));
  browser::stopTasks();
  browser::stepTasks();
  assert(executions == 16);
  puts(
      "PASS scheduler: notifications, timeouts, mutex contention, recursion, binary semaphores, deletion, pool reuse, "
      "stop");
}
