#pragma once
#include <EInkDisplay.h>
#include <EpdFontFamily.h>

#include <string>
#include <utility>

#include "../Activity.h"

class FullScreenMessageActivity final : public Activity {
  std::string text;
  EpdFontStyle style;
  EInkDisplay::RefreshMode refreshMode;

 public:
  explicit FullScreenMessageActivity(GfxRenderer& renderer, InputManager& inputManager, std::string text,
                                     const EpdFontStyle style = REGULAR,
                                     const EInkDisplay::RefreshMode refreshMode = EInkDisplay::FAST_REFRESH)
      : Activity(renderer, inputManager), text(std::move(text)), style(style), refreshMode(refreshMode) {}
  void onEnter() override;
};
