#pragma once

#include <Epub/Page.h>
#include <I18n.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Text shown in a framed popup over the reading page. Shared chrome for the
// dictionary popup and the footnote popup: frame with a bold title row,
// wrapped scrolling text, Back or a tap outside the frame dismisses, buttons
// and in-frame taps scroll. Subclasses feed text via showText(); the
// dictionary popup drives its lookup phase first and delegates here once the
// article is ready.
class TextPopupActivity : public Activity {
 public:
  TextPopupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Page> page,
                    const int marginLeft, const int marginTop, std::string title, std::string text,
                    const char* activityName = "TextPopup");

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 protected:
  // Layout and display `text` under `title`; false on heap/geometry failure
  // (payload cleared). Once true, the base loop/render take over.
  bool showText(std::string title, std::string text);
  bool textReady() const { return !lines.empty(); }

  // Background page under the frame (may be null: plain frame).
  std::unique_ptr<Page> page;
  const int marginLeft;
  const int marginTop;
  int fontId = 0;

 private:
  void layoutFrame();
  bool wrapText();
  void drawPopupBody();
  void scrollText(int direction);

  std::string title;
  std::string text;
  std::vector<std::pair<uint32_t, uint16_t>> lines;  // (start, len) into text
  int frameX = 0, frameY = 0, frameW = 0, frameH = 0;
  int linesPerPage = 1;
  int scrollLine = 0;

  ButtonNavigator buttonNavigator;
};
