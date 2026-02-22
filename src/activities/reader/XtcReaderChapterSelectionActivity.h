#pragma once
#include <Xtc.h>

#include <memory>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

class XtcReaderChapterSelectionActivity final : public Activity {
  std::shared_ptr<Xtc> xtc;
  ButtonNavigator buttonNavigator;
  uint32_t currentPage = 0;
  int selectorIndex = 0;

  const std::function<void()> onGoBack;
  const std::function<void(uint32_t newPage)> onSelectPage;

  int getPageItems() const;
  int findChapterIndexForPage(uint32_t page) const;

 public:
  explicit XtcReaderChapterSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             const std::shared_ptr<Xtc>& xtc, uint32_t currentPage,
                                             const std::function<void()>& onGoBack,
                                             const std::function<void(uint32_t newPage)>& onSelectPage)
      : Activity("XtcReaderChapterSelection", renderer, mappedInput),
        xtc(xtc),
        currentPage(currentPage),
        onGoBack(onGoBack),
        onSelectPage(onSelectPage) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;
};
