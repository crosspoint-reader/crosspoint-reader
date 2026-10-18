#pragma once
#include <mutex>
using StaticSemaphore_t = std::mutex;
using SemaphoreHandle_t = std::mutex*;
constexpr unsigned portMAX_DELAY = ~0u;
inline auto xSemaphoreCreateMutexStatic(StaticSemaphore_t* p) { return p; }
inline void xSemaphoreTake(SemaphoreHandle_t p, unsigned) { p->lock(); }
inline void xSemaphoreGive(SemaphoreHandle_t p) { p->unlock(); }
