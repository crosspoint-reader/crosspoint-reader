#pragma once

class Activity;  // forward declaration

// RAII helper to lock rendering mutex for the duration of a scope.
class RenderLock {
  bool isLocked = false;

 public:
  // Tag type selecting the constructor below. Named rather than a bool so call sites
  // read as a statement of intent instead of `RenderLock lock(true)`.
  struct ExclusiveActivityAccess {};

  // Try takes the mutex only if it is free at this instant and never waits; check ownsLock()
  // before touching anything the lock guards. It is for loop-task code that must read state
  // the render task owns but would rather skip a tick than stall input behind a whole render
  // pass. It answers "is the mutex free", not "is a pass in flight": the render task drops the
  // mutex mid-pass (see ExclusiveActivityAccess below), so a Try can succeed during a pass.
  enum class Mode { Blocking, Try };

  explicit RenderLock(Mode mode = Mode::Blocking);
  explicit RenderLock(Activity&);  // unused for now, but keep for compatibility

  // Acquire the rendering mutex AND guarantee the render task is not inside
  // currentActivity->render(). Use this — never the plain constructor — before
  // destroying or replacing the current activity.
  //
  // Holding the mutex alone is NOT enough: the render task deliberately drops it in the
  // middle of a pass (renderContents() releases it before the waveform wait so the loop
  // task can service input and schedule a pre-render) and then re-acquires it and keeps
  // dereferencing the activity. A transition that took the plain lock in that window
  // could run the activity's destructor while the render task was still using it.
  //
  // Blocks until both conditions hold. See ActivityManager::renderPassActive.
  explicit RenderLock(ExclusiveActivityAccess);

  RenderLock(const RenderLock&) = delete;
  RenderLock& operator=(const RenderLock&) = delete;
  ~RenderLock();
  bool ownsLock() const { return isLocked; }
  void unlock();
  static bool peek();
  // True when the calling task holds renderingMutex: the debug check for code that relies on it.
  static bool heldByCurrentTask();
};
