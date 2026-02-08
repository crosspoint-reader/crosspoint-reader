#pragma once

#include <GfxRenderer.h>
#include <MappedInputManager.h>

#include <functional>
#include <vector>

#include "../../extension/AppLoader.h"
#include "../Activity.h"

class AppsActivity : public Activity {
 public:
  using ExitCallback = std::function<void()>;

  AppsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, ExitCallback exitCallback);

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  GfxRenderer& renderer_;
  MappedInputManager& mappedInput_;
  ExitCallback exitCallback_;

  std::vector<CrossPoint::AppInfo> appList_;
  int selectedIndex_;
  bool needsUpdate_;
  bool isFlashing_;
  int flashProgress_;
  bool flashingStarted_;

  void scanApps();
  void launchApp();
  void render();
  void renderProgress();
  void renderBooting();
  void showProgress(size_t written, size_t total);
};
