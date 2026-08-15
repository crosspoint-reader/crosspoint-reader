#pragma once

class Activity;  // forward declaration

// RAII helper to lock rendering mutex for the duration of a scope.
class RenderLock {
  bool isLocked = false;

 public:
  explicit RenderLock();
  explicit RenderLock(Activity&);  // unused for now, but keep for compatibility
#ifdef CROSSPOINT_BG_BUILD_TASK
  // Non-blocking acquisition for background tasks. A background task must
  // NEVER park on the rendering mutex: ActivityManager calls onExit() while
  // holding the RenderLock, and onExit joins the background build task — a
  // task blocked inside the blocking ctor at that moment would deadlock the
  // exit. Check locked() before touching guarded state; on false, back off
  // and retry the whole iteration (the stop flag is re-read each pass).
  struct TryAcquire {};
  explicit RenderLock(TryAcquire);
  bool locked() const { return isLocked; }
#endif
  RenderLock(const RenderLock&) = delete;
  RenderLock& operator=(const RenderLock&) = delete;
  ~RenderLock();
  void unlock();
  static bool peek();
};
