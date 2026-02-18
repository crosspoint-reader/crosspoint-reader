#pragma once

#include "activities/Activity.h"
#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "util/ButtonNavigator.h"
#include <functional>

class TrmnlSetupActivity : public Activity {
 public:
  TrmnlSetupActivity(GfxRenderer& renderer, MappedInputManager& inputManager, std::function<void()> onBack);
  void onEnter() override;
  void loop() override;
  void onExit() override;
  void render(Activity::RenderLock&& lock) override;

 private:
  std::function<void()> onBack;
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
  std::string statusMessage;
  bool displayingImage = false;
  
  void toggleEnabled();
  void performRegistration();
  void testDownload();
};
