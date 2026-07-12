#pragma once
#include <functional>
#include <vector>
#include "MappedInputManager.h"

class ButtonNavigator {
 public:
  void onNext(std::function<void()> cb) {}
  void onPrevious(std::function<void()> cb) {}
  void onNextRelease(std::function<void()> cb) {}
  void onPreviousRelease(std::function<void()> cb) {}
  void onNextContinuous(std::function<void()> cb) {}
  void onPreviousContinuous(std::function<void()> cb) {}
  void onPressAndContinuous(std::vector<MappedInputManager::Button>, std::function<void()>) {}

  static int nextIndex(int current, int count) {
    return (current + 1) % count;
  }
  static int previousIndex(int current, int count) {
    return (current - 1 + count) % count;
  }
  static int nextPageIndex(int current, int count, int pageSize) {
    int next = current + pageSize;
    return (next >= count) ? count - 1 : next;
  }
  static int previousPageIndex(int current, int count, int pageSize) {
    int prev = current - pageSize;
    return (prev < 0) ? 0 : prev;
  }
};
