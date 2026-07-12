#pragma once
#include <memory>
#include <string>
#include <utility>
#include "ActivityManager.h"
#include "ActivityResult.h"
#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "RenderLock.h"

class Activity {
 protected:
  std::string name;
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;
  ActivityResultHandler resultHandler;
  ActivityResult result;

 public:
  explicit Activity(std::string name, GfxRenderer& r, MappedInputManager& m)
      : name(std::move(name)), renderer(r), mappedInput(m) {}
  virtual ~Activity() = default;
  virtual void onEnter() {}
  virtual void onExit() {}
  virtual void loop() {}
  virtual void render(RenderLock&&) {}
  virtual void requestUpdate(bool = false) {}
  virtual void requestUpdateAndWait() {}
  virtual bool skipLoopDelay() { return false; }
  virtual bool preventAutoSleep() { return false; }
  virtual bool isReaderActivity() const { return false; }

  void startActivityForResult(std::unique_ptr<Activity>&&, ActivityResultHandler) {}
  void setResult(ActivityResult&& r) { result = std::move(r); }
  void finish() {}
  void onGoHome() {}
  void onSelectBook(const std::string&) {}
};
