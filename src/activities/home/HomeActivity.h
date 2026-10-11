#pragma once

#include <BuildArena.h>
#include <PngToBmpConverter.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../Activity.h"
#include "./FileBrowserActivity.h"
#include "CoverThumbLoader.h"
#include "Epub/CoverThumbSession.h"
#include "HomeMenu.h"
#include "ReadingStats.h"
#include "RecentBooksStore.h"
#include "activities/reader/ReaderActivity.h"
#include "components/UITheme.h"
#include "util/ButtonNavigator.h"

struct RecentBook;
struct Rect;

class HomeActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  int lastCarouselBookIndex = 0;  // remembered position when leaving carousel row
  bool recentsLoading = false;
  bool recentsLoaded = false;
  bool firstRenderDone = false;
  bool coverRendered = false;
  bool coverBufferStored = false;
  // True while the ~52 KB secondary framebuffer is LENT to the cold-cache cover pipeline as
  // its scratch region (coverScratch_): the OPF inflate ring, the cover extraction ring and the
  // decoders' working memory bump-allocate inside it instead of the heap. It used to be
  // RELEASED for the same headroom; a task stack or a book-lifetime block landing in the freed
  // hole then left the realloc short and the device without its buffer (memory audit 2026-09).
  // The lent block never enters the heap, and the return cannot fail. Returned once all covers
  // are loaded, and on exit. See loadRecentCovers() / restoreSecondaryBuffer().
  bool secondaryBufferLent = false;
  // The lent region itself. During the cover pass it backs coverScratch_; once every cover is
  // resolved the carousel keeps it as its frame cache (frameCacheInRegion_) instead of handing
  // it back, and it returns to the display on exit or before a child activity opens.
  uint8_t* lentRegion_ = nullptr;
  size_t lentRegionBytes_ = 0;
  bool frameCacheInRegion_ = false;
  std::unique_ptr<BuildArena> coverScratch_;
  size_t nextRecentCoverIndex = 0;

  // Makes the covers of the book at nextRecentCoverIndex -- every carousel size from one decode
  // where the cover allows, sliced so a press pauses it (memory audit 2026-09, R9 items 2 and 3).
  // Its sessions hold their state in coverScratch_ between slices (declared after it, so it goes
  // first), and it removes partial thumbnails of work it does not finish.
  CoverThumbLoader coverLoader;
  int coverLoaderHeight_ = -1;  // the cover height the loader is configured for

  // Session-scoped transient-failure counter, keyed by book path. A cover can fail to load for
  // transient reasons (OOM under heap pressure, an interrupted write, an extraction that could not
  // start). We retry such a book on later passes, but bounded: after COVER_MAX_TRANSIENT_ATTEMPTS
  // transient failures in one Home session we stop retrying it THIS session (store an empty cover
  // and advance) so a persistently-failing book can't starve the others. No persistent sentinel is
  // written for transient failures — a reboot resets the map and gives every book a fresh chance;
  // only a structurally-absent cover (no cover item / unsupported format) earns a permanent sentinel
  // (written by generateThumbBmp). Cleared in onEnter().
  static constexpr uint8_t COVER_MAX_TRANSIENT_ATTEMPTS = 2;
  std::unordered_map<std::string, uint8_t> coverTransientAttempts;

  uint8_t* coverBuffer = nullptr;
  size_t coverBufferSize = 0;
  int coverRectX = 0;
  int coverRectY = 0;
  int coverRectW = 0;
  int coverRectH = 0;

  std::vector<RecentBook> recentBooks;
  // The recent-books store stays loaded while Home is open: it refreshes the list on entry and
  // writes each cover it makes back into it (updateBook), and reloading the file for each of
  // those would cost far more than the ~4.5 KB it holds here. Released on exit, so the reader
  // does not carry it.
  std::optional<RecentBooksStore::Hold> recentsHold;
  std::vector<HomeMenuEntry> menuEntries;
  bool menuEntriesDirty = true;

  std::string focusBookPath;
  int focusSelectorIndex = -1;

  void onSelectBook(const std::string& path);
  // Tap on a cover or a menu entry: opens it. Returns true when the touch was consumed.
  // Inert on non-touch boards. Reads the targets the active theme published.
  bool handleHomeTouch();
  void dispatchMenuAction(HomeMenuAction action);

  void rebuildMenuEntries();
  bool keepRegionAsFrameCache();
  bool storeCoverBuffer();
  bool restoreCoverBuffer();
  void freeCoverBuffer();
  void restoreSecondaryBuffer(bool callerHoldsRenderLock = false);
  void loadRecentBooks(int maxBooks);
  void loadRecentCovers(int coverHeight);
  // One thumbnail slot to placeholder on a permanent give-up: the exact on-disk path the cover
  // loader checks, plus the (w,h) the placeholder BMP should be written at.
  struct ThumbSlot {
    std::string path;
    int width;
    int height;
  };
  // Give up on a book's cover for this pass. Bumps the session retry counter for a transient
  // failure. When the failure is permanent — structurally absent, or transient but past the
  // session retry budget — writes a valid placeholder BMP at each slot (as CoverThumbLoader does)
  // so the book reads as resolved on disk and is not re-decoded on the next boot; otherwise just
  // records an empty cover so it retries next session. Shared by both cover paths.
  void giveUpCover(RecentBook& book, ThumbResult res, const std::vector<ThumbSlot>& slots);
  // True once a book has burned COVER_MAX_TRANSIENT_ATTEMPTS transient failures this session.
  bool coverAttemptsExhausted(const std::string& path) const;

 public:
  explicit HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string focusBookPath = {},
                        int focusSelectorIndex = -1)
      : Activity("Home", renderer, mappedInput),
        focusBookPath(std::move(focusBookPath)),
        focusSelectorIndex(focusSelectorIndex) {}
  void onEnter() override;
  void onExit() override;

  void loop() override;
  void render(RenderLock&&) override;
  // A child drawn over Home (the touch boards' light drawer) recovers the displayed frame from the
  // secondary buffer, which does nothing while Home has it lent: hand it back first.
  void startActivityForResult(std::unique_ptr<Activity>&& activity, ActivityResultHandler resultHandler) override;
  // Covers still resolving (not just mid-pass): loadRecentCovers() clears recentsLoading at
  // every yield point so loop() re-enters it, which briefly makes the activity look idle. If
  // skipLoopDelay went false in that window, the main loop's inactivity governor could drop
  // the CPU to 10 MHz mid-burst and the next decode tick would crawl (observed: a ~1.5 s
  // cover decode taking ~25 s). Hold full speed until every recent cover is resolved.
  bool skipLoopDelay() override { return (firstRenderDone && !recentsLoaded) || recentsLoading || coverLoader.busy(); }
};
