#pragma once
#include <Logging.h>

#include <cassert>
#include <string>
#include <utility>

#include "ActivityManager.h"
#include "GfxRenderer.h"
#include "MappedInputManager.h"

class ActivityResult;  // forward declaration
class RenderLock;      // forward declaration

class Activity {
 protected:
  std::string name;
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;

 public:
  // Will be set by ActivityManager when pushActivityForResult is used
  std::function<void(ActivityResult&)> resultHandler;

  explicit Activity(std::string name, GfxRenderer& renderer, MappedInputManager& mappedInput)
      : name(std::move(name)), renderer(renderer), mappedInput(mappedInput) {}
  virtual ~Activity() = default;
  virtual void onEnter();
  virtual void onExit();
  virtual void loop() {}

  virtual void render(RenderLock&&) {}
  virtual void requestUpdate();
  virtual void requestUpdateAndWait();

  virtual bool skipLoopDelay() { return false; }
  virtual bool preventAutoSleep() { return false; }
  virtual bool isReaderActivity() const { return false; }

  // Convenience method to facilitate API transition to ActivityManager
  // TODO: remove this in near future
  void onGoHome();
  void onSelectBook(const std::string& path);
};
