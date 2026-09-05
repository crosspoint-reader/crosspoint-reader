#pragma once
#include <EpdFontFamily.h>
#include <HalDisplay.h>

#include <string>
#include <utility>

#include "activities/Activity.h"

class FullScreenMessageActivity final : public Activity {
  std::string text;
  EpdFontFamily::Style style;
  HalDisplay::RefreshMode refreshMode;
  // False for existing callers (e.g. the SD-card-error screen): a genuine
  // dead end with no working storage to go back to. True lets Back/Confirm
  // dismiss back to whatever pushed this activity via startActivityForResult.
  bool dismissible;

 public:
  explicit FullScreenMessageActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string text,
                                     const EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                                     const HalDisplay::RefreshMode refreshMode = HalDisplay::FAST_REFRESH,
                                     const bool dismissible = false)
      : Activity("FullScreenMessage", renderer, mappedInput),
        text(std::move(text)),
        style(style),
        refreshMode(refreshMode),
        dismissible(dismissible) {}
  void onEnter() override;
  void loop() override;
};
