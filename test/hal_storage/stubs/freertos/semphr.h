// Host stand-in for FreeRTOS's semphr.h: a recursive mutex that only counts how deep it is held, so
// a test can check that every StorageLock is given back. The tests run on one thread.
#pragma once
#include "FreeRTOS.h"

using SemaphoreHandle_t = int*;

inline int& hostSemaphoreDepth() {
  static int depth = 0;
  return depth;
}
inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex() {
  static int mutex = 0;
  return &mutex;
}
inline BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t, TickType_t) {
  ++hostSemaphoreDepth();
  return 1;
}
inline BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t) {
  --hostSemaphoreDepth();
  return 1;
}
