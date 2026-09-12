#pragma once

#include <StorageTestSupport.h>

#include <algorithm>
#include <cassert>

using SemaphoreHandle_t = int*;
constexpr unsigned portMAX_DELAY = ~0U;

inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex() { return &storage_test::state.lockDepth; }

inline bool xSemaphoreTakeRecursive(SemaphoreHandle_t mutex, unsigned) {
  auto& state = storage_test::state;
  ++*mutex;
  ++state.lockTakes;
  state.maxLockDepth = std::max(state.maxLockDepth, *mutex);
  return true;
}

inline bool xSemaphoreGiveRecursive(SemaphoreHandle_t mutex) {
  assert(*mutex > 0);
  --*mutex;
  ++storage_test::state.lockGives;
  return true;
}
