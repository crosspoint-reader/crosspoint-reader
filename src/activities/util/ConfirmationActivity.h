#pragma once
#include <functional>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "fontIds.h"

class ConfirmationActivity : public Activity {
 private:
  // Input data
  std::string heading;
  std::string body;

  const int margin = 20;
  const int spacing = 30;
  const int fontId = UI_10_FONT_ID;
  // Five, not six: the text starts at screenHeight / 6 to stay clear of the
  // centred confirmation popup, and the block grows downward from there. On an
  // X4 a heading plus six lines reaches the popup's top edge; five clears it by
  // roughly 20px.
  static constexpr int maxBodyLines = 5;

  std::string safeHeading;
  std::vector<std::string> bodyLines;
  OptionPopup confirmPopup;
  int startY = 0;
  int lineHeight = 0;

 public:
  ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& heading,
                       const std::string& body);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&& lock) override;
};