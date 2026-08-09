#pragma once

#include "activities/Activity.h"

/// Base class for Rust activities
/// Handles all lifecycle bridging between C++ and Rust.
/// Each Rust activity subclasses this.
class ActivityRs : public Activity {
 protected:
  /// Opaque pointer to Rust activity object
  void* rust_activity = nullptr;

  /// Points the FFI globals at this activity. Called on entry and again at
  /// the top of every loop() and render(), so a screen resumed from the
  /// activity stack regains them without a second onEnter().
  void bindGlobals();

  /// Factory function type for creating Rust activities
  using CreateFn = void* (*)();

  /// Constructor - takes a factory function to create the Rust activity
  explicit ActivityRs(const char* name, GfxRenderer& renderer, MappedInputManager& mappedInput, CreateFn create_fn);

 public:
  ~ActivityRs() override;

  /// Localized title shown in the navigation header. Subclasses override this
  /// with a tr() lookup so translation stays on the C++ side; it is read fresh
  /// on every render, so a language change is picked up immediately.
  virtual const char* getTitle() const { return name.c_str(); }

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handleHomeGesture() override;

 private:
  CreateFn create_fn;

  /// Free heap when the screen was entered, so onExit can report the delta.
  uint32_t heapOnEnter = 0;
};
