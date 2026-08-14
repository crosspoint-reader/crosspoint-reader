#pragma once

#include <Txt.h>

#include <memory>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "ReaderDocument.h"

class TxtReaderDocument final : public ReaderDocument {
  std::unique_ptr<Txt> txt;

  int currentPage = 0;
  int totalPages = 1;

  // Streaming text reader - stores file offsets for each page
  std::vector<size_t> pageOffsets;
  std::vector<std::string> currentPageLines;
  int linesPerPage = 0;
  int viewportWidth = 0;
  bool initialized = false;

  // Cached settings for cache validation
  int cachedFontId = 0;
  uint8_t cachedScreenMargin = 0;
  uint8_t cachedParagraphAlignment = CrossPointSettings::LEFT_ALIGN;
  int cachedOrientedMarginTop = 0;
  int cachedOrientedMarginRight = 0;
  int cachedOrientedMarginBottom = 0;
  int cachedOrientedMarginLeft = 0;

  void renderPage(GfxRenderer& renderer);
  void initializeReader(GfxRenderer& renderer);
  bool loadPageAtOffset(GfxRenderer& renderer, size_t offset, std::vector<std::string>& outLines, size_t& nextOffset);
  void buildPageIndex(GfxRenderer& renderer);
  bool loadPageIndexCache();
  void savePageIndexCache() const;
  void saveProgress() const;
  void loadProgress();

 public:
  explicit TxtReaderDocument(ReaderActivity& host, std::unique_ptr<Txt> txt)
      : ReaderDocument(host), txt(std::move(txt)) {}
  ~TxtReaderDocument() override = default;

  const std::string& getPath() const override {
    static const std::string empty;
    return txt ? txt->getPath() : empty;
  }
  std::string getTitle() const override { return txt ? txt->getTitle() : ""; }

  bool load(bool allowFastInitialRefresh) override;
  void render(ReaderRenderContext& context) override;

  bool pageTurn(bool isForward) override;
  bool skipPages(int amount) override;
  bool isAtEndOfBook() const override;
  void onReturnFromEndOfBook() override;

  bool rendersOwnStatusBar() const override { return false; }
  bool commitsDisplayBuffer() const override { return false; }
  void renderStatusBar(GfxRenderer& renderer) const override;

  ScreenshotInfo getScreenshotInfo() const override;
};
