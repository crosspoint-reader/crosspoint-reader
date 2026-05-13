#pragma once

#include <Txt.h>
#include <Typesetter/SectionCache.h>

#include "CrossPointSettings.h"
#include "activities/Activity.h"

class Page;

class TxtReaderActivity final : public Activity {
  std::unique_ptr<Txt> txt;

  int currentPage = 0;
  int pagesUntilFullRefresh = 0;
  bool initialized = false;

  SectionCache cache;

  // Cached settings for cache validation (different fonts/margins require re-indexing)
  int cachedFontId = 0;
  int cachedOrientedMarginTop = 0;
  int cachedOrientedMarginRight = 0;
  int cachedOrientedMarginBottom = 0;
  int cachedOrientedMarginLeft = 0;

  void renderContents(std::unique_ptr<Page> page);
  void renderStatusBar() const;

  void initializeReader();
  bool buildSectionCache(uint16_t viewportWidth, uint16_t viewportHeight);
  void saveProgress() const;
  void loadProgress();

 public:
  explicit TxtReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Txt> txt)
      : Activity("TxtReader", renderer, mappedInput),
        txt(std::move(txt)),
        cache(this->txt->getCachePath() + "/section.bin") {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  ScreenshotInfo getScreenshotInfo() const override;
};
