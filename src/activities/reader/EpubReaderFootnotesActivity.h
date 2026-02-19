#pragma once

#include <Epub/FootnoteEntry.h>

#include <cstring>
#include <functional>
#include <vector>

#include "../ActivityWithSubactivity.h"
#include "util/ButtonNavigator.h"

class EpubReaderFootnotesActivity final : public ActivityWithSubactivity {
 public:
  explicit EpubReaderFootnotesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                       const std::vector<FootnoteEntry>& footnotes,
                                       const std::function<void()>& onGoBack,
                                       const std::function<void(const char*)>& onSelectFootnote)
      : ActivityWithSubactivity("EpubReaderFootnotes", renderer, mappedInput),
        footnotes(footnotes),
        onGoBack(onGoBack),
        onSelectFootnote(onSelectFootnote) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;

 private:
  const std::vector<FootnoteEntry>& footnotes;
  const std::function<void()> onGoBack;
  const std::function<void(const char*)> onSelectFootnote;
  int selectedIndex = 0;
  int scrollOffset = 0;
  ButtonNavigator buttonNavigator;
};
