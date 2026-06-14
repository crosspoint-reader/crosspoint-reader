#pragma once
#include <functional>
#include <string>

#include "activities/Activity.h"
#include "fontIds.h"

class ConfirmationActivity : public Activity {
 private:
  // Input data
  std::string heading;
  std::string body;

  // When true, render as a centred dialog box composited over the caller's screen instead of
  // clearing to a full-screen prompt. Off by default so existing callers are unaffected.
  const bool overlay;

  const int margin = 20;
  const int spacing = 30;
  const int fontId = UI_10_FONT_ID;

  std::string safeHeading;
  std::string safeBody;
  int startY = 0;
  int lineHeight = 0;
  // True when Confirm was already held as this dialog opened (e.g. carried over from the
  // selection that launched it); swallow that first release so it can't auto-confirm.
  bool lockNextConfirmRelease = false;
  // Overlay box geometry (only used when overlay == true), computed in onEnter().
  int boxX = 0;
  int boxY = 0;
  int boxW = 0;
  int boxH = 0;

 public:
  ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& heading,
                       const std::string& body, bool overlay = false);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&& lock) override;
};