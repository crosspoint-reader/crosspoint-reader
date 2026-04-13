#pragma once
#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/Section.h>

#include <optional>

#include "EpubReaderMenuActivity.h"
#include "ReaderUtils.h"
#include "activities/Activity.h"

class EpubReaderActivity final : public Activity {
  // Reader can launch sync in three UX modes:
  // - COMPARE: legacy chooser (apply/upload) for power users.
  // - PULL_REMOTE / PUSH_LOCAL: direct one-step actions from menu entries.
  // Keeping this split in the caller avoids branching on menu semantics deep
  // inside generic reader state handling.
  enum class SyncLaunchMode {
    COMPARE,
    PULL_REMOTE,
    PUSH_LOCAL,
  };

  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section = nullptr;
  int currentSpineIndex = 0;
  int nextPageNumber = 0;
  // Set when navigating to a TOC entry in a different spine (chapter skip or chapter selector).
  // Cleared on the next render after the new section loads and resolves it to a page.
  std::optional<int> pendingTocIndex;
  // Set when navigating to a footnote href with a fragment (e.g. #note1).
  // Cleared on the next render after the new section loads and resolves it to a page.
  std::string pendingAnchor;
  int pagesUntilFullRefresh = 0;
  int cachedSpineIndex = 0;
  int cachedChapterTotalPageCount = 0;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  // Signals that the next render should reposition within the newly loaded section
  // based on a cross-book percentage jump.
  bool pendingPercentJump = false;
  // Normalized 0.0-1.0 progress within the target spine item, computed from book percentage.
  float pendingSpineProgress = 0.0f;
  // Pending paragraph index from KOReader sync (resolved to page via Section paragraph LUT)
  bool pendingParagraphLookup = false;
  uint16_t pendingParagraphIndex = 0;
  bool pendingScreenshot = false;
  bool skipNextButtonCheck = false;  // Skip button processing for one frame after subactivity exit
  ReaderUtils::InputDrainGuard inputDrainGuard;
  bool automaticPageTurnActive = false;
  // -1 means use global SETTINGS value.
  int8_t bookEmbeddedStyleOverride = -1;
  int8_t bookImageRenderingOverride = -1;

  // Footnote support
  std::vector<FootnoteEntry> currentPageFootnotes;
  struct SavedPosition {
    int spineIndex;
    int pageNumber;
  };
  static constexpr int MAX_FOOTNOTE_DEPTH = 3;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;

  void renderContents(std::unique_ptr<Page> page, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft);
  void renderStatusBar() const;
  void silentIndexNextChapterIfNeeded(uint16_t viewportWidth, uint16_t viewportHeight);
  void saveProgress(int spineIndex, int currentPage, int pageCount);
  // Jump to a percentage of the book (0-100), mapping it to spine and page.
  void jumpToPercent(int percent);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  void launchKOReaderSync(SyncLaunchMode mode = SyncLaunchMode::COMPARE);
  // Consume a persisted standalone KOReader sync session for this EPUB. Remote
  // apply writes the mapped reopen position into progress.bin before the normal
  // reader startup path reads it. Upload-complete leaves the existing local
  // progress.bin untouched and simply clears the pending session marker.
  void applyPendingSyncSession();
  void applyOrientation(uint8_t orientation);
  void applyTextDarkness(uint8_t textDarkness);
  void toggleAutoPageTurn(uint8_t selectedPageTurnOption);
  void applyBookReaderOverrides(int8_t embeddedStyleOverride, int8_t imageRenderingOverride);
  bool getEffectiveEmbeddedStyle() const;
  uint8_t getEffectiveImageRendering() const;
  void pageTurn(bool isForwardTurn);

  // Footnote navigation
  void navigateToHref(const std::string& href, bool savePosition = false);
  void restoreSavedPosition();

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Epub> epub)
      : Activity("EpubReader", renderer, mappedInput), epub(std::move(epub)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  bool isReaderActivity() const override { return true; }

  // Renders the last saved page to the frame buffer without flushing to display.
  // Used by SleepActivity to prepare the background for the overlay sleep mode.
  // Returns false if the page cannot be loaded (missing cache / file error).
  static bool drawCurrentPageToBuffer(const std::string& filePath, GfxRenderer& renderer);
};
