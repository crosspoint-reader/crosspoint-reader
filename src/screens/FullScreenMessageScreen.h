#pragma once
#include <string>
#include <utility>

#include <EInkDisplay.h>
#include <EpdFontFamily.h>
#include "Screen.h"

class FullScreenMessageScreen final : public Screen {
  std::string text;
  EpdFontStyle style;
  EInkDisplay::RefreshMode refreshMode;

 public:
  explicit FullScreenMessageScreen(EpdRenderer& renderer, InputManager& inputManager, std::string text,
                                   const EpdFontStyle style = REGULAR,
                                   const EInkDisplay::RefreshMode refreshMode = EInkDisplay::FAST_REFRESH)
      : Screen(renderer, inputManager),
        text(std::move(text)),
        style(style),
        refreshMode(refreshMode) {}
  void onEnter() override;
};
