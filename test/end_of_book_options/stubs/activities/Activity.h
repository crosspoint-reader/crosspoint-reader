#pragma once

#include "components/UiAppHost.h"

struct RenderLock {};
struct ActivityManager {
  std::string openedPath;
  int homes = 0;
  void goToReader(const std::string& path) { openedPath = path; }
};
inline ActivityManager activityManager;

class Activity {
 protected:
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;

 public:
  int updates = 0;
  Activity(const char*, GfxRenderer& renderer, MappedInputManager& input) : renderer(renderer), mappedInput(input) {}
  virtual ~Activity() = default;
  virtual void onEnter() {}
  virtual void onExit() {}
  virtual void loop() {}
  virtual void render(RenderLock&&) {}
  virtual bool isReaderActivity() const { return false; }
  virtual bool handleForcedRefresh() { return false; }
  void requestUpdate() { ++updates; }
  static void onGoHome() { ++activityManager.homes; }
};
