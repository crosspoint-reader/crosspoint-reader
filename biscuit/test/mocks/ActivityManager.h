#pragma once
#include <memory>
#include <string>
#include "GfxRenderer.h"
#include "MappedInputManager.h"

class Activity;

class ActivityManager {
 public:
  GfxRenderer renderer;
  MappedInputManager mappedInput;
  ActivityManager(GfxRenderer& r, MappedInputManager& m) : renderer(r), mappedInput(m) {}
  ActivityManager() : renderer(defaultRenderer), mappedInput(defaultInput) {}

  void pushActivity(std::unique_ptr<Activity>&&) {}
  void popActivity() {}
  void replaceActivity(std::unique_ptr<Activity>&&) {}
  void goHome() {}
  void goToReader(std::string) {}
  void goToFileBrowser(std::string = {}) {}
  void requestUpdate(bool = false) {}
  void requestUpdateAndWait() {}

 private:
  GfxRenderer defaultRenderer;
  MappedInputManager defaultInput;
};

// Global singleton (defined in test main)
extern ActivityManager activityManager;
