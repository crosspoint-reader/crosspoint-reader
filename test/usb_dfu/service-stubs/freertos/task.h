#pragma once
#include <thread>
inline std::thread taskThread;
inline bool failAllocation = false;
inline int xTaskCreate(void (*fn)(void*), const char*, unsigned, void* arg, unsigned, void*) {
  if (failAllocation) return 0;
  taskThread = std::thread(fn, arg);
  return 1;
}
inline void vTaskDelete(void*) {}
