#pragma once
#include <string>
#include <vector>

#include "../Activity.h"

class ClippingTextViewerActivity final : public Activity {
  std::string text;
  std::vector<std::string> lines;  // text split into screen-width lines
  int scrollOffset = 0;            // first visible line index
  int linesPerPage = 0;

  void wrapText();  // Split text into lines that fit screen width

  void render(RenderLock&&) override;

 public:
  explicit ClippingTextViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string text)
      : Activity("ClippingTextViewer", renderer, mappedInput), text(std::move(text)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
