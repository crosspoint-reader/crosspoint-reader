#pragma once
#include <EpdFontFamily.h>
#include <HalDisplay.h>

#include <string>
#include <utility>

#include "../Activity.h"

class FullScreenMessageActivity final : public Activity {
  std::string text;
  EpdFontFamily::Style style;
  HalDisplay::RefreshMode refreshMode;

 public:
  explicit FullScreenMessageActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string text,
                                     const EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                                     const HalDisplay::RefreshMode refreshMode = HalDisplay::FAST_REFRESH)
      : Activity("FullScreenMessage", renderer, mappedInput),
        text(std::move(text)),
        style(style),
        refreshMode(refreshMode) {}
  void onEnter() override;
};
