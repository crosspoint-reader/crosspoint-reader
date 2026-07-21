#pragma once

#include <cstdint>
#include <string>

#include "ReadingStatsPresentation.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class ReadingStatsActivity final : public Activity {
 public:
  ReadingStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookTitle,
                       ReadingStatsPresentation presentation);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Page : uint8_t { Book, Device, AllSynced };

  int pageCount() const { return presentation.showAllSynced ? 3 : 2; }

  std::string bookTitle;
  ReadingStatsPresentation presentation;
  ButtonNavigator buttonNavigator;
  Page page = Page::Book;
};
