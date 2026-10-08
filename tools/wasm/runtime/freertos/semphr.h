#pragma once

#include "FreeRTOS.h"

struct BrowserSemaphore;
using SemaphoreHandle_t = BrowserSemaphore*;
SemaphoreHandle_t xSemaphoreCreateMutex();
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex();
SemaphoreHandle_t xSemaphoreCreateBinary();
void vSemaphoreDelete(SemaphoreHandle_t sem);
bool xSemaphoreTake(SemaphoreHandle_t sem, uint32_t ticksToWait);
bool xSemaphoreGive(SemaphoreHandle_t sem);
TaskHandle_t xSemaphoreGetMutexHolder(SemaphoreHandle_t sem);
int xQueuePeek(SemaphoreHandle_t sem, void*, uint32_t ticksToWait);
inline bool xSemaphoreTakeRecursive(SemaphoreHandle_t sem, uint32_t ticks) { return xSemaphoreTake(sem, ticks); }
inline bool xSemaphoreGiveRecursive(SemaphoreHandle_t sem) { return xSemaphoreGive(sem); }
