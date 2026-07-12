#pragma once

#include <Epub/FootnoteEntry.h>

#include <cstring>
#include <functional>
#include <vector>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

class EpubReaderFootnotesActivity final : public Activity {
 public:
  explicit EpubReaderFootnotesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                       const std::vector<FootnoteEntry>& footnotes)
      : Activity("EpubReaderFootnotes", renderer, mappedInput), footnotes(footnotes) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  const std::vector<FootnoteEntry>& footnotes;
  int selectedIndex = 0;
  int scrollOffset = 0;
  ButtonNavigator buttonNavigator;
};
