#pragma once

#include <GfxRenderer.h>
#include <MdParser.h>
#include <Txt.h>

#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "Activity.h"
#include "activities/reader/EpubReaderMenuActivity.h"

class MdReaderActivity final : public Activity {
  std::unique_ptr<Txt> txt;

  int currentPage = 0;
  int totalPages = 1;
  int pagesUntilFullRefresh = 0;

  const std::function<void()> onGoBack;
  const std::function<void()> onGoHome;

  // Streaming reader — stores file byte offsets and code-fence state for each page start.
  std::vector<size_t> pageOffsets;
  std::vector<bool> pageCodeFences;
  std::vector<uint8_t> pageSubLineStarts;  ///< First sub-line index to emit for each page (for wrapped lines)
  std::vector<MdLine> currentPageLines;
  int linesPerPage = 0;
  int viewportWidth = 0;
  bool initialized = false;

  // Cached settings for cache validation (different fonts/margins require re-indexing)
  int cachedFontId = 0;
  int cachedScreenMargin = 0;
  uint8_t cachedParagraphAlignment = CrossPointSettings::LEFT_ALIGN;

  void renderPage();
  void renderStatusBar(int orientedMarginRight, int orientedMarginBottom, int orientedMarginLeft) const;

  void initializeReader();
  bool loadPageAtOffset(size_t offset, int subLineStart, std::vector<MdLine>& outLines, size_t& nextOffset,
                        int& nextSubLineStart, bool& inCodeFence, bool stripInline = true);
  void buildPageIndex();
  bool loadPageIndexCache();
  void savePageIndexCache() const;
  void saveProgress() const;
  void loadProgress();

 public:
  explicit MdReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Txt> txt,
                            const std::function<void()>& onGoBack, const std::function<void()>& onGoHome)
      : Activity("MdReader", renderer, mappedInput),
        txt(std::move(txt)),
        onGoBack(onGoBack),
        onGoHome(onGoHome) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool supportsOrientation() const override { return true; }
  void applyOrientation(uint8_t orientation);
};
