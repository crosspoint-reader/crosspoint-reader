#pragma once
#include <condition_variable>
#include <mutex>
#include <thread>

#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY 0xFFFFFFFF
#define eIncrement 1
#define portTICK_PERIOD_MS 1

// ESP-IDF's portMUX_TYPE is a spinlock used with taskENTER_CRITICAL /
// taskEXIT_CRITICAL to guard data shared between tasks (and, on multi-core
// targets, cores). The simulator has no real critical-section primitive, so
// back it with a real mutex to preserve the same mutual-exclusion semantics
// across host threads.
struct SimPortMux {
  std::recursive_mutex mtx;
};
typedef SimPortMux portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED                                          \
  {}

inline void taskENTER_CRITICAL(portMUX_TYPE *mux) { mux->mtx.lock(); }
inline void taskEXIT_CRITICAL(portMUX_TYPE *mux) { mux->mtx.unlock(); }
#define portENTER_CRITICAL(mux) taskENTER_CRITICAL(mux)
#define portEXIT_CRITICAL(mux) taskEXIT_CRITICAL(mux)

// TaskHandle wraps a real thread + a notification counter protected by a
// condvar.
struct SimTaskHandle {
  std::thread thread;
  std::mutex mtx;
  std::condition_variable cv;
  uint32_t notifyCount = 0;
  std::thread::id id;
  const char *name = "sim-task";
};
typedef SimTaskHandle *TaskHandle_t;
