#pragma once

#include <string>
#include <functional>
#include "../Activity.h"
#include "MappedInputManager.h"

class BmpViewerActivity final : public Activity {
public:
  
  BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, 
                    std::string filePath, 
                    std::function<void()> onGoHome,
                    std::function<void()> onGoBack);

  void onEnter() override;
  void onExit() override;
  void loop() override;

private:
  std::string filePath;
  std::function<void()> onGoHome;
  std::function<void()> onGoBack;
  
  static constexpr uint32_t goHomeMs = 1000;
};