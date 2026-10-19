#pragma once
#include <atomic>
struct Manager {
  std::atomic<unsigned> paints{0};
  void requestUpdateAndWait() { paints++; }
};
inline Manager activityManager;
