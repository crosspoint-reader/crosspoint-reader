#pragma once
#include <CrossPointFont.h>
#include <HalDisplay.h>

#include <string>
#include <utility>

#include "../Activity.h"

class FullScreenMessageActivity final : public Activity {
  std::string text;
  CrossPointFont::Style style;
  HalDisplay::RefreshMode refreshMode;

 public:
  explicit FullScreenMessageActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string text,
                                     const CrossPointFont::Style style = CrossPointFont::REGULAR,
                                     const HalDisplay::RefreshMode refreshMode = HalDisplay::FAST_REFRESH)
      : Activity("FullScreenMessage", renderer, mappedInput),
        text(std::move(text)),
        style(style),
        refreshMode(refreshMode) {}
  void onEnter() override;
};
