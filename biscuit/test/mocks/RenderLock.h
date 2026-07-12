#pragma once
class Activity;
class RenderLock {
 public:
  RenderLock() {}
  explicit RenderLock(Activity&) {}
  ~RenderLock() {}
  void unlock() {}
  static bool peek() { return false; }
};
