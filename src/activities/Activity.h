#pragma once
#include <Logging.h>

#include <cassert>
#include <memory>
#include <string>
#include <utility>

#include "ActivityManager.h"  // for using the ActivityManager singleton
#include "ActivityResult.h"
#include "ButtonEventManager.h"
#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "RenderLock.h"

class Activity {
  friend class ActivityManager;

 protected:
  std::string name;
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;
  ButtonEventManager& buttonEvents;

  ActivityResultHandler resultHandler;
  ActivityResult result;

 public:
  explicit Activity(std::string name, GfxRenderer& renderer, MappedInputManager& mappedInput)
      : name(std::move(name)), renderer(renderer), mappedInput(mappedInput), buttonEvents(globalButtonEvents()) {}
  virtual ~Activity() = default;
  const std::string& getName() const { return name; }
  virtual void onEnter();
  virtual void onExit();
  virtual void loop() {}

  virtual void render(RenderLock&&) {}

  // If immediate is true, the update will be triggered immediately.
  // Otherwise, it will be deferred until the end of the current loop iteration.
  virtual void requestUpdate(bool immediate = false);

  // Request an immediate render and block until it completes.
  virtual void requestUpdateAndWait();

  virtual bool skipLoopDelay() { return false; }
  virtual bool preventAutoSleep() { return false; }
  virtual bool isReaderActivity() const { return false; }

  // Return true to suppress the minute-tick requestUpdate() from ActivityManager when nothing
  // status-bar-relevant has changed since the last render. Skipping avoids a no-op page render
  // followed by a no-diff e-ink refresh, which on X3 panels accumulates visible speckle.
  virtual bool shouldSkipPeriodicUpdate() const { return false; }

  // Called by ActivityManager when a globally-configured button action targets the
  // current activity. Override in reader activities to handle reader-specific actions.
  // Non-reader activities can ignore this (default is no-op).
  virtual void onButtonAction(CrossPointSettings::BUTTON_ACTION) {}

  // Start a new activity without destroying the current one
  // Note: requestUpdate() will be invoked automatically once resultHandler finishes
  void startActivityForResult(std::unique_ptr<Activity>&& activity, ActivityResultHandler resultHandler);

  // Set the result to be passed back to the previous activity when this activity finishes
  void setResult(ActivityResult&& result);

  // Finish this activity and return to the previous one on the stack (if any)
  void finish();

  // Convenience method to facilitate API transition to ActivityManager
  // TODO: remove this in near future
  void onGoHome();
};
