#pragma once

class Activity;  // forward declaration

// RAII helper to lock rendering mutex for the duration of a scope.
class RenderLock {
  bool isLocked = false;

 public:
  explicit RenderLock();
  explicit RenderLock(Activity&);  // unused for now, but keep for compatibility
  // Non-blocking acquisition for deferrable work. The loop task's background
  // chores (deferred build starts, the build pump) must not park on the
  // rendering mutex: losing the peek->acquire race would block the loop behind
  // an entire page render, stalling input polling for its duration. Check
  // locked() before touching guarded state; on false, retry the next pass.
  struct TryAcquire {};
  explicit RenderLock(TryAcquire);
  bool locked() const { return isLocked; }
  RenderLock(const RenderLock&) = delete;
  RenderLock& operator=(const RenderLock&) = delete;
  ~RenderLock();
  void unlock();
  static bool peek();
};
