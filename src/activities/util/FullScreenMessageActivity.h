#pragma once
#include <EInkDisplay.h>
#include <EpdFontFamily.h>

#include <string>
#include <utility>

#include "../Activity.h"

class FullScreenMessageActivity final : public Activity {
  std::string text;
  EpdFontFamily::Style style;
  EInkDisplay::RefreshMode refreshMode;

 public:
  explicit FullScreenMessageActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string text,
                                     const EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                                     const EInkDisplay::RefreshMode refreshMode = EInkDisplay::FAST_REFRESH)
      : Activity("FullScreenMessage", renderer, mappedInput),
        text(std::move(text)),
        style(style),
        refreshMode(refreshMode) {}
  void onEnter() override;
};
