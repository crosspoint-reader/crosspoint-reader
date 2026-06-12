#pragma once
#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/Page.h>
#include <Epub/Section.h>

#include <optional>
#include <vector>

#include "EpubReaderMenuActivity.h"
#include "HighlightEntry.h"
#include "ProgressMapper.h"
#include "activities/Activity.h"

class EpubReaderActivity final : public Activity {
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section = nullptr;
  int currentSpineIndex = 0;
  int nextPageNumber = 0;
  std::optional<uint16_t> pendingPageJump;
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
  bool pendingScreenshot = false;
  bool pendingSyncSaveError = false;
  bool skipNextButtonCheck = false;  // Skip button processing for one frame after subactivity exit
  bool automaticPageTurnActive = false;
  bool showBookmarkMessage = false;
  bool ignoreNextConfirmRelease = false;
  // Tracks whether this book is currently removed from Recent Books by the
  // removeReadBooksFromRecents feature (set at End-of-Book, cleared if paged back in).
  bool recentsEntryRemoved = false;
  unsigned long bookmarkMessageTime = 0UL;
  // Set when the reader is left at end-of-book and SETTINGS.moveFinishedToReadFolder is on.
  // Consumed in onExit() to relocate the finished book into /Read/.
  bool pendingReadFolderMove = false;

  // Footnote support
  std::vector<FootnoteEntry> currentPageFootnotes;
  struct SavedPosition {
    int spineIndex;
    int pageNumber;
  };
  static constexpr int MAX_FOOTNOTE_DEPTH = 3;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;

  void renderContents(Page& page, int orientedMarginTop, int orientedMarginRight, int orientedMarginBottom,
                      int orientedMarginLeft);
  void renderStatusBar() const;
  void silentIndexNextChapterIfNeeded(uint16_t viewportWidth, uint16_t viewportHeight);
  bool saveProgress(int spineIndex, int currentPage, int pageCount);
  // Jump to a percentage of the book (0-100), mapping it to spine and page.
  void jumpToPercent(int percent);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  void applyOrientation(uint8_t orientation);
  void toggleAutoPageTurn(uint8_t selectedPageTurnOption);
  void pageTurn(bool isForwardTurn);
  void addBookmark();

  // ---- Text highlighting / selection ----
  // On-screen geometry of one selectable word on the current page. Coordinates
  // are logical (orientation-aware), already offset by the page margins.
  struct SelectableWord {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
    uint16_t element;  // index into Page::elements
    uint16_t word;     // index into the TextBlock's words for that element
  };
  bool selectionMode = false;
  bool selectionAnchorSet = false;
  int selectionCursor = 0;  // index into selectableWords
  // Anchor (first endpoint) captured on the first Confirm. Page/element/word are
  // resolved to a flat ordering against the cursor when finalizing.
  struct SelectionEndpoint {
    uint16_t page;
    uint16_t element;
    uint16_t word;
  };
  SelectionEndpoint selectionAnchor = {};
  std::vector<SelectableWord> selectableWords;  // rebuilt per page while selecting
  // Page kept loaded while in selection mode so cursor-move redraws reuse it
  // instead of re-reading the section file from SD on every step. No new
  // allocation — just deferred free of the already-loaded page; released in
  // exitSelectionMode().
  std::unique_ptr<Page> selectionPageCache;
  uint16_t selectionPageCacheNum = 0;
  // Highlights for the current section, loaded on section load for re-display.
  std::vector<HighlightEntry> sectionHighlights;
  bool sectionHighlightsLoaded = false;
  int loadedHighlightsSpine = -1;
  bool showHighlightMessage = false;
  unsigned long highlightMessageTime = 0UL;
  bool showHighlightDeletedMessage = false;
  bool showHighlightOverlapMessage = false;
  // Set after a long-press delete fires so the following Confirm release does
  // not also set an anchor / finalize a selection.
  bool ignoreNextSelectionConfirmRelease = false;

  void enterSelectionMode();
  void exitSelectionMode();
  void buildSelectableWords(const Page& page, int marginLeft, int marginTop);
  void handleSelectionInput();
  void finalizeHighlight();
  // Delete the stored highlight under the selection cursor (long-press Confirm).
  // Returns true if a highlight was found and removed.
  bool deleteHighlightAtCursor();
  void loadSectionHighlights();
  // Draw stored highlights (underline) for the given page; best-effort.
  void drawStoredHighlights(const Page& page, int marginLeft, int marginTop) const;
  // Draw the in-progress selection overlay (anchor..cursor underline + cursor box).
  void drawSelectionOverlay() const;

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
  ScreenshotInfo getScreenshotInfo() const override;
  CrossPointPosition getCurrentPosition() const;
};
