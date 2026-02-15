#pragma once

#include <Logging.h>

#include <string>
#include <utility>

class MappedInputManager;
class GfxRenderer;

class Activity {
 protected:
  std::string name;
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;

 public:
  explicit Activity(std::string name, GfxRenderer& renderer, MappedInputManager& mappedInput)
      : name(std::move(name)), renderer(renderer), mappedInput(mappedInput) {}
  virtual ~Activity() = default;
  virtual void onEnter() { LOG_DBG("ACT", "Entering activity: %s", name.c_str()); }
  virtual void onExit() { LOG_DBG("ACT", "Exiting activity: %s", name.c_str()); }
  virtual void loop() {}
  virtual bool skipLoopDelay() { return false; }
  virtual bool preventAutoSleep() { return false; }
  virtual bool isReaderActivity() const { return false; }
};
