#include "HomeActivity.h"

#include <Bitmap.h>
#include <CooperativeAbort.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <Memory.h>
#include <PngToBmpConverter.h>
#include <Txt.h>
#include <Utf8.h>
#include <Xtc.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "GlobalBookmarkIndex.h"
#include "KOReaderDocumentId.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "ReadingStats.h"
#include "RecentBooksStore.h"
#include "TouchUi.h"
#include "activities/reader/ReaderActivity.h"
#include "components/UITheme.h"
#include "components/themes/TapTargets.h"
#include "components/themes/lyra/Lyra3CoversTheme.h"
#include "fontIds.h"

namespace {
constexpr int CLASSIC_MIN_RECENT_TILE_HEIGHT = 280;
constexpr int LYRA_MIN_RECENT_TILE_HEIGHT = 170;
constexpr int LYRA_3_COVERS_MIN_RECENT_TILE_HEIGHT = 200;
constexpr int CLASSIC_MIN_RECENT_TO_MENU_GAP = 2;
constexpr int LYRA_MIN_RECENT_TO_MENU_GAP = 4;

struct HomeScreenLayout {
  int recentTileHeight;
  int recentToMenuGap;
  int menuHeight;
};

bool isLyraFamilyTheme() {
  const auto theme = static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme);
  return theme == CrossPointSettings::UI_THEME::LYRA || theme == CrossPointSettings::UI_THEME::LYRA_3_COVERS;
}

bool isLyraExtendedTheme() {
  return static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA_3_COVERS;
}

int getMinRecentTileHeight() {
  if (isLyraExtendedTheme()) {
    return LYRA_3_COVERS_MIN_RECENT_TILE_HEIGHT;
  }
  if (isLyraFamilyTheme()) {
    return LYRA_MIN_RECENT_TILE_HEIGHT;
  }
  return CLASSIC_MIN_RECENT_TILE_HEIGHT;
}

int getMinRecentToMenuGap() {
  return isLyraFamilyTheme() ? LYRA_MIN_RECENT_TO_MENU_GAP : CLASSIC_MIN_RECENT_TO_MENU_GAP;
}

HomeScreenLayout computeHomeScreenLayout(const ThemeMetrics& metrics, int contentHeight, int menuItemCount) {
  HomeScreenLayout layout{metrics.homeCoverTileHeight, metrics.verticalSpacing, 0};

  const int menuRequiredHeight =
      menuItemCount * metrics.menuRowHeight + std::max(0, menuItemCount - 1) * metrics.menuSpacing;

  auto computeMenuHeight = [&]() {
    return contentHeight - (metrics.homeTopPadding + layout.recentTileHeight + layout.recentToMenuGap);
  };

  layout.menuHeight = computeMenuHeight();
  if (layout.menuHeight >= menuRequiredHeight) {
    return layout;
  }

  const int gapReduction =
      std::min(layout.recentToMenuGap - getMinRecentToMenuGap(), menuRequiredHeight - layout.menuHeight);
  if (gapReduction > 0) {
    layout.recentToMenuGap -= gapReduction;
    layout.menuHeight = computeMenuHeight();
  }

  if (layout.menuHeight >= menuRequiredHeight) {
    return layout;
  }

  const int tileReduction =
      std::min(layout.recentTileHeight - getMinRecentTileHeight(), menuRequiredHeight - layout.menuHeight);
  if (tileReduction > 0) {
    layout.recentTileHeight -= tileReduction;
    layout.menuHeight = computeMenuHeight();
  }

  layout.menuHeight = std::max(0, layout.menuHeight);
  return layout;
}

int getHomeCoverRenderHeight(const HomeScreenLayout& layout) {
  // Lyra Extended's split between cover and text lives with the theme that draws it: the height
  // chosen here goes into the thumbnail's filename, so the theme has to ask for the same one.
  return isLyraExtendedTheme() ? Lyra3CoversMetrics::coverRenderHeight(layout.recentTileHeight)
                               : std::max(120, layout.recentTileHeight - (isLyraFamilyTheme() ? 16 : 0));
}
}  // namespace

// Builds the menu entry list in display order. Single source of truth for both loop() (which
// dispatches Confirm based on action) and render() (which draws labels/icons).
void HomeActivity::rebuildMenuEntries() {
  menuEntries.clear();
  menuEntries.reserve(6);  // all six shown, or five plus More
  const HomeMenuAvailability availability{.hasBookmarks = !GLOBAL_BOOKMARKS.isEmpty()};
  collectHomeMenuEntries(HomeMenuPlacement::Home, availability, menuEntries);
  menuEntriesDirty = false;
}

void HomeActivity::loadRecentBooks(int maxBooks) {
  recentBooks.clear();
  RECENT_BOOKS.refreshSidecarMetadata(static_cast<size_t>(std::max(0, maxBooks)));
  const auto& books = RECENT_BOOKS.getBooks();
  recentBooks.reserve(std::min(static_cast<int>(books.size()), maxBooks));

  for (const RecentBook& book : books) {
    // Limit to maximum number of recent books
    if (recentBooks.size() >= maxBooks) {
      break;
    }

    if (RecentBooksStore::isMissing(book)) {
      continue;
    }

    // Check for a sidecar cover — takes priority over embedded cover.
    // Also catches books registered before sidecar support (empty coverBmpPath).
    const std::string sidecar = ReaderActivity::sidecarCoverPath(book.path);
    if (!sidecar.empty()) {
      const std::string bookCacheDir = ReaderActivity::bookCacheDir(book.path);
      const bool sidecarAlreadyStored =
          book.coverBmpPath == sidecar || (book.coverBmpPath.rfind(bookCacheDir + "/", 0) == 0 &&
                                           book.coverBmpPath.find("[HEIGHT]") != std::string::npos);
      LOG_DBG("HOME", "Sidecar for %s: stored=%s alreadyStored=%d", book.path.c_str(), book.coverBmpPath.c_str(),
              sidecarAlreadyStored ? 1 : 0);
      if (!sidecarAlreadyStored) {
        LOG_DBG("HOME", "Updating coverBmpPath to sidecar: %s", sidecar.c_str());
        RECENT_BOOKS.updateBook(book.path, book.title, book.author, book.series, sidecar);
        RecentBook updated = book;
        updated.coverBmpPath = sidecar;
        recentBooks.push_back(updated);
        continue;
      }
    }

    recentBooks.push_back(book);
  }
}

bool HomeActivity::coverAttemptsExhausted(const std::string& path) const {
  const auto it = coverTransientAttempts.find(path);
  return it != coverTransientAttempts.end() && it->second >= COVER_MAX_TRANSIENT_ATTEMPTS;
}

void HomeActivity::giveUpCover(RecentBook& book, ThumbResult res, const std::vector<ThumbSlot>& slots) {
  // Only transient failures count toward the session budget; a structural absence is already
  // permanent (generateThumbBmp wrote a sentinel) and needs no retry accounting.
  if (res == ThumbResult::TransientFail) {
    const uint8_t n = ++coverTransientAttempts[book.path];
    LOG_DBG("HOME", "Transient cover failure %u/%u for %s", n, COVER_MAX_TRANSIENT_ATTEMPTS, book.path.c_str());
  }

  // Permanent give-up = structurally absent, or transient failures past this session's budget
  // (e.g. an embedded cover the decoder rejects every time — oversize PNG, corrupt image). As
  // CoverThumbLoader does: write a valid placeholder BMP at each thumb slot so isCoverThumbComplete()
  // treats the book as resolved on disk and it is never re-decoded on the next boot. A still-retryable
  // transient failure records an empty cover instead, so it gets a fresh attempt next session.
  const bool permanent = (res == ThumbResult::StructurallyAbsent) || coverAttemptsExhausted(book.path);

  if (permanent) {
    bool allWritten = !slots.empty();
    for (const auto& slot : slots) {
      if (!ReaderActivity::isCoverThumbComplete(slot.path, slot.width, slot.height) &&
          !ReaderActivity::writeCoverPlaceholderBmp(slot.path)) {
        allWritten = false;
      }
    }
    if (allWritten) {
      const std::string placeholder = ReaderActivity::coverThumbPlaceholder(book.path);
      LOG_DBG("HOME", "Wrote cover placeholder(s) for %s — resolved, will not re-decode", book.path.c_str());
      RECENT_BOOKS.updateBook(book.path, book.title, book.author, book.series, placeholder);
      book.coverBmpPath = placeholder;
      return;
    }
    // Placeholder write failed (e.g. tight heap) — fall through to empty; retry next pass.
    LOG_DBG("HOME", "Placeholder write failed for %s — recording empty, will retry", book.path.c_str());
  }

  RECENT_BOOKS.updateBook(book.path, book.title, book.author, book.series, "");
  book.coverBmpPath = "";
}

void HomeActivity::loadRecentCovers(int coverHeight) {
  recentsLoading = true;

  // Cold-cache cover loading runs the JPEG/PNG decoders, the sliced ZIP inflate and
  // an EPUB parse — together these can drive free heap to the edge of OOM (observed
  // Min Free ~2.8 KB) while the ~48 KB secondary framebuffer sits unused. The reader
  // releases that buffer before decoding for the same reason; do the same here for the
  // duration of loading and reallocate it once every cover is resolved (see end of this
  // function) or on exit. Release under the render lock so we never free it mid-render.
  if (secondaryBufferLent && frameCacheInRegion_) {
    // A later pass on the same visit: the carousel's frame cache has the region; the decoders
    // need it back. (The cache would be rebuilt anyway -- new covers are about to land.)
    RenderLock lock;
    UITheme::getInstance().getMutableTheme().setFrameCacheRegion(nullptr, 0);
    frameCacheInRegion_ = false;
    coverScratch_ = makeUniqueNoThrow<BuildArena>(lentRegion_, lentRegionBytes_);
    if (!coverScratch_ || !coverScratch_->valid()) coverScratch_.reset();
  } else if (!secondaryBufferLent && renderer.hasSecondaryBuffer()) {
    RenderLock lock;
    // The lend hands out the DISPLAYED frame and leaves the write buffer as the only one, and
    // after the home render's swap that buffer holds the frame from two refreshes ago -- the
    // boot screen, or whatever screen preceded Home. Unless Home repaints before it exits, the
    // return below seeds the secondary from that stale frame, and the next overlay (the reader's
    // "Indexing" popup, the cold-font popup, anything drawn with drawPopup) lands on it. Copy the
    // displayed frame in while the secondary still holds it.
    renderer.syncWriteBufferFromDisplayed();
    // Seed RED RAM with the displayed frame too, which the SDK requires before single-buffer fast
    // diff. A two-buffer FAST loads RED from the previous frame when it STARTS and leaves it
    // there, so RED now holds whatever preceded Home. Unseeded, the first refresh after the lend
    // -- a carousel move, or Settings' entry frame when Home exits without redrawing -- skips
    // every pixel the new frame shares with that older screen, and Home's ink stays on the glass
    // there. Runs before the borrow, while the secondary still holds the displayed frame.
    renderer.syncRedRamFromFrameBuffer();
    size_t lentSize = 0;
    if (uint8_t* lent = renderer.borrowSecondaryBuffer(&lentSize)) {
      coverScratch_ = makeUniqueNoThrow<BuildArena>(lent, lentSize);
      if (coverScratch_ && coverScratch_->valid()) {
        secondaryBufferLent = true;
        lentRegion_ = lent;
        lentRegionBytes_ = lentSize;
        // Keep X4 fast-differential refresh alive while the secondary buffer is lent: RED RAM
        // was seeded with the home frame above and the driver re-seeds it after every
        // single-buffer refresh, so carousel/menu navigation diffs
        // against that baseline instead of downgrading to a full/half waveform on every press.
        // Precondition holds: we lend right after the first home render (gate requires
        // firstRenderDone) and only issue plain BW redraws until the return. No-op on X3.
        renderer.setSingleBufferFastDiff(true);
        LOG_DBG("HOME", "Lent secondary framebuffer for cover loading (%u bytes, free=%lu)",
                static_cast<unsigned>(lentSize), static_cast<unsigned long>(esp_get_free_heap_size()));
      } else {
        coverScratch_.reset();
        renderer.returnSecondaryBuffer();
      }
    }
  }

  // A frame being drawn shares the core (on the C3 the render task has the loop task's priority)
  // and the SD card with this pass. Bursting beside it stretched the redraw a press asked for
  // from ~360 ms to 0.6-1.1 s (X3, 2026-09-27): the sliced sessions resume after a press instead
  // of restarting, so -- unlike the one-shot decode they replaced -- they were still running when
  // the redraw began. Step aside until the frame is handed to the panel; the ~430 ms refresh
  // after that needs neither, and the pass keeps that window.
  const auto frameInProgress = [this]() { return renderer.isComposingFrame(); };
  if (frameInProgress()) {
    recentsLoading = false;
    delay(2);  // block rather than spin: a spinning loop task still takes its share of the core
    return;
  }

  const auto thumbSizes = GUI.getCoverThumbSizes(coverHeight);

  // Build the placeholder slots for a book on give-up. Multi-size themes placeholder every
  // WxH thumb; the single-height path placeholders its one thumb_<h>.bmp (width = h*0.6, matching
  // the decode). Each slot's path is exactly what the loader's isCoverThumbComplete() checks.
  const auto slotsForBook = [&](const std::string& placeholder) {
    std::vector<ThumbSlot> slots;
    if (!thumbSizes.empty()) {
      slots.reserve(thumbSizes.size());
      std::transform(thumbSizes.begin(), thumbSizes.end(), std::back_inserter(slots), [&](const auto& sz) {
        return ThumbSlot{UITheme::getCoverThumbPath(placeholder, sz.first, sz.second), sz.first, sz.second};
      });
    } else {
      slots.push_back({UITheme::getCoverThumbPath(placeholder, coverHeight), coverHeight * 6 / 10, coverHeight});
    }
    return slots;
  };

  // HomeActivity::loop runs on the main task while rendering runs on the render task.
  // Invalidate the Lyra frame cache under the render lock to avoid freeing cached
  // frames while tryFastHomeRender is copying from them.
  const auto invalidateFrameCacheSafely = []() {
    RenderLock lock;
    UITheme::getInstance().getMutableTheme().invalidateFrameCache();
  };

  // Called at every early-return point where a cover was just written to disk.
  // Marks the carousel frame cache dirty so the next tryFastHomeRender rebuilds
  // with the new BMP.  Uses markFrameCacheDirty() rather than invalidateFrameCache()
  // so multiple covers arriving between renders coalesce into one SD re-read
  // instead of triggering a full cache rebuild (3 BMP file opens) for each book.
  const auto yieldAfterDecode = [this]() {
    RenderLock lock;
    UITheme::getInstance().getMutableTheme().markFrameCacheDirty();
    coverRendered = false;
    recentsLoading = false;
    requestUpdate();
  };

  // Time budget for a single loadRecentCovers() call. Rather than advancing a session by
  // one small slice per loop() tick (a 1200x1848 PNG cover needs ~940 ZIP-inflate chunks +
  // ~308 decode-row batches ≈ 1250 ticks — minutes of wall-clock), we drain slices in a
  // burst until this budget elapses or button input arrives. Browse Files' cover pass
  // bursts the same way. A slice only writes SD files (no screen change), so
  // bursting costs nothing visually; we still yield promptly to keep input responsive.
  constexpr uint32_t COVER_SLICE_BUDGET_MS = 150;
  // The pass's thumbnails: the carousel's two boxes, cropped, or the single-height form the other
  // themes draw. Set once per height, not per call: configure() abandons the book in hand.
  if (coverLoaderHeight_ != coverHeight) {
    if (thumbSizes.empty()) {
      coverLoader.configureSingleHeight(coverHeight);
    } else {
      coverLoader.configure(thumbSizes.data(), static_cast<int>(thumbSizes.size()), /*crop=*/true);
    }
    coverLoaderHeight_ = coverHeight;
  }

  // One book at a time, through the shared CoverThumbLoader: every size of it from one decode where
  // the cover allows (R9 item 2), sliced so a press pauses the work where it stands (item 3). This
  // screen keeps only what is its own: the order of the books, the per-session retry budget, the
  // recent-books record, and the frame cache.
  while (true) {
    if (!coverLoader.busy()) {
      // The next book whose cover is not on the card yet.
      for (; nextRecentCoverIndex < recentBooks.size(); nextRecentCoverIndex++) {
        RecentBook& book = recentBooks[nextRecentCoverIndex];
        if (!Storage.exists(book.path.c_str())) continue;
        if (!coverLoader.complete(book.path)) break;
        // Already present -- make sure the stored path is the canonical placeholder so a stale
        // "[HEIGHT].bmp" / raw-sidecar entry self-heals to the unified naming without a re-decode.
        const std::string placeholder = ReaderActivity::coverThumbPlaceholder(book.path);
        if (book.coverBmpPath != placeholder) {
          LOG_DBG("HOME", "Self-heal: coverBmpPath=%s -> placeholder=%s", book.coverBmpPath.c_str(),
                  placeholder.c_str());
          RECENT_BOOKS.updateBook(book.path, book.title, book.author, book.series, placeholder);
          book.coverBmpPath = placeholder;
        }
      }
      if (nextRecentCoverIndex >= recentBooks.size()) break;  // every cover resolved

      // Button input has priority: never start a decode while a press is queued, or while the
      // frame it asked for is being drawn. The next pass picks the same book up.
      if (mappedInput.hasPendingInput() || frameInProgress()) {
        recentsLoading = false;
        return;
      }
      RecentBook& book = recentBooks[nextRecentCoverIndex];
      // This book already burned its transient-failure budget this session -- stop retrying it (a
      // reboot resets the counter and tries again) so it cannot starve the others.
      if (coverAttemptsExhausted(book.path)) {
        LOG_DBG("HOME", "Cover attempts exhausted for %s this session -- writing placeholder", book.path.c_str());
        giveUpCover(book, ThumbResult::StructurallyAbsent,
                    slotsForBook(ReaderActivity::coverThumbPlaceholder(book.path)));
        nextRecentCoverIndex++;
        yieldAfterDecode();
        return;
      }
      // Cover decode needs ~42 KB contiguous heap -- free the frame cache first.
      invalidateFrameCacheSafely();
      coverLoader.begin(book.path, coverScratch_.get());
    }

    // A burst on the book in hand. A step only writes SD files, so the screen is redrawn once,
    // when the book is resolved -- not per step.
    const uint32_t deadline = millis() + COVER_SLICE_BUDGET_MS;
    CoverThumbLoader::Step step = CoverThumbLoader::Step::Working;
    while ((step = coverLoader.step()) == CoverThumbLoader::Step::Working) {
      if (mappedInput.hasPendingInput() || frameInProgress() || static_cast<int32_t>(millis() - deadline) >= 0) {
        recentsLoading = false;  // resume where it stands on the next call
        return;
      }
    }
    RecentBook& book = recentBooks[nextRecentCoverIndex];
    const std::string placeholder = ReaderActivity::coverThumbPlaceholder(book.path);
    if (step == CoverThumbLoader::Step::Done) {
      // A cover written, or the placeholder of a book that has none: the stored path is canonical.
      if (book.coverBmpPath != placeholder) {
        RECENT_BOOKS.updateBook(book.path, book.title, book.author, book.series, placeholder);
        book.coverBmpPath = placeholder;
      }
    } else {
      // Counts the transient; writes a durable placeholder once the budget is spent.
      giveUpCover(book, ThumbResult::TransientFail, slotsForBook(placeholder));
    }
    nextRecentCoverIndex++;
    yieldAfterDecode();
    return;
  }

  recentsLoaded = true;
  recentsLoading = false;
  if (!keepRegionAsFrameCache()) restoreSecondaryBuffer();
}

bool HomeActivity::keepRegionAsFrameCache() {
  // Every cover is resolved, so the region would go back to the display now -- where, on the
  // carousel, it leaves Home without a frame cache: the cache wants one ~49 KB block (X3; 45 KB
  // X4), which Home's heap never has with the buffer resident (run 16: "cover region 0 (49104
  // bytes, 36108 free)" on every render, then "OOM: cover buffer (20592 bytes)" for the fallback),
  // so every render redrew the three covers from SD: ~360 ms against ~40 ms restored from a cache.
  // The region is idle for the rest of the visit, so the cache lives there instead. The display
  // keeps working as it does during the cover pass (single-buffer fast diff).
  if (!secondaryBufferLent || lentRegion_ == nullptr || frameCacheInRegion_) return false;
  if (coverLoader.busy()) return false;  // nothing may still hold a block
  auto& theme = UITheme::getInstance().getMutableTheme();
  const size_t wanted = theme.frameCacheRegionBytes(renderer);
  if (wanted == 0 || wanted > lentRegionBytes_) return false;
  RenderLock lock;
  coverScratch_.reset();
  theme.setFrameCacheRegion(lentRegion_, lentRegionBytes_);
  frameCacheInRegion_ = true;
  // The fallback cover buffer (20 592 B on the X3) is dead weight from here on: the fast path
  // serves every render. Left allocated it held Home at ~28 KB free for the rest of the visit
  // instead of ~49 KB (X3, 2026-09-27). If the region goes back early the fallback re-stores it.
  freeCoverBuffer();
  LOG_DBG("HOME", "Kept the lent framebuffer as the carousel frame cache (%u of %u bytes, free=%lu)",
          static_cast<unsigned>(wanted), static_cast<unsigned>(lentRegionBytes_),
          static_cast<unsigned long>(esp_get_free_heap_size()));
  return true;
}

void HomeActivity::startActivityForResult(std::unique_ptr<Activity>&& activity, ActivityResultHandler resultHandler) {
  restoreSecondaryBuffer(/*callerHoldsRenderLock=*/false);
  Activity::startActivityForResult(std::move(activity), std::move(resultHandler));
}

void HomeActivity::restoreSecondaryBuffer(bool callerHoldsRenderLock) {
  // Reallocate the framebuffer we released for cover loading. Best-effort: if malloc
  // fails (heap still tight) we stay degraded and retry on the next call — rendering
  // tolerates a missing secondary buffer (full/half refresh instead of fast AA).
  //
  // The RenderLock is non-recursive. onExit() already runs UNDER the lock (held by
  // ActivityManager::exitActivity), so it must pass callerHoldsRenderLock=true — taking
  // a second RenderLock there self-deadlocks and hangs the Home→Reader transition. The
  // end-of-loading caller runs from loop() with no lock held and passes false.
  if (!secondaryBufferLent) return;
  const auto doRestore = [this]() {
    // Anything still holding a block in the lent region goes first: an abandoned extract, PNG or
    // JPEG session at exit would otherwise release into a region the display owns again.
    coverLoader.reset();
    coverScratch_.reset();
    if (frameCacheInRegion_) {
      UITheme::getInstance().getMutableTheme().setFrameCacheRegion(nullptr, 0);
      frameCacheInRegion_ = false;
    }
    renderer.returnSecondaryBuffer();  // cannot fail: the region never entered the heap
    secondaryBufferLent = false;
    lentRegion_ = nullptr;
    lentRegionBytes_ = 0;
    // Two-buffer differential is available again — turn off the single-buffer RED-RAM-baseline
    // mode so normal fast refresh resumes against the secondary. No syncRedRamFromFrameBuffer()
    // here: the return re-seeds the baseline exactly as a realloc does, and RED already holds the
    // home frame -- seeded at the lend, and re-seeded by every single-buffer refresh since.
    renderer.setSingleBufferFastDiff(false);
    LOG_DBG("HOME", "Returned secondary framebuffer after cover loading (free=%lu contig=%lu)",
            static_cast<unsigned long>(esp_get_free_heap_size()),
            static_cast<unsigned long>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT)));
  };
  if (callerHoldsRenderLock) {
    doRestore();
  } else {
    RenderLock lock;
    doRestore();
  }
}

void HomeActivity::onEnter() {
  Activity::onEnter();
  recentsHold.emplace();

  // A finished-book "sync to KOReader, then search OPDS for this author" request that just
  // rebooted (the sync reboots to reclaim WiFi-session heap fragmentation, see
  // KOReaderSyncActivity::onExit()) lands here first — there's no reader to hand it to the way
  // the OpenBook/Reader post-actions can via APP_STATE.openEpubPath, so Home is what consumes it.
  // replaceActivity() defers this to the next loop() tick rather than re-entering while this
  // activity is still being constructed (see the "delete this" guard in ActivityManager).
  auto& pendingSync = APP_STATE.koReaderSyncSession;
  if (pendingSync.postAction == KOReaderSyncPostAction::OpdsSearch) {
    const std::string author = pendingSync.postActionTarget;
    pendingSync.clear();
    APP_STATE.saveToFile();
    if (!author.empty() && OPDS_STORE.hasServers()) {
      activityManager.goToBrowserWithSearch(author);
      return;
    }
  }

  selectorIndex = 0;
  recentsLoading = false;
  recentsLoaded = false;
  firstRenderDone = false;
  nextRecentCoverIndex = 0;
  coverLoader.reset();
  coverTransientAttempts.clear();
  coverRendered = false;
  secondaryBufferLent = false;
  lentRegion_ = nullptr;
  lentRegionBytes_ = 0;
  frameCacheInRegion_ = false;
  freeCoverBuffer();

  const auto& metrics = UITheme::getInstance().getMetrics();
  loadRecentBooks(metrics.homeRecentBooksCount);
  // The themes draw each recent book's history and pace from the stats cache; fill it here, on the
  // loop task, so their render() reads no file. A warm cache makes this free.
  std::vector<std::string> recentDocIds;
  recentDocIds.reserve(recentBooks.size());
  for (const auto& book : recentBooks) recentDocIds.push_back(KOReaderDocumentId::calculateFromFilename(book.path));
  READING_STATS.prefetchRecent(recentDocIds);
  if (recentBooks.empty()) {
    recentsLoaded = true;
  }

  // Apply focus: book path takes priority, else combined selector index (covers
  // "return to the menu entry I was on").
  bool focused = false;
  if (!focusBookPath.empty()) {
    for (size_t i = 0; i < recentBooks.size(); ++i) {
      if (recentBooks[i].path == focusBookPath) {
        selectorIndex = static_cast<int>(i);
        focused = true;
        break;
      }
    }
    focusBookPath.clear();
  }
  if (!focused && focusSelectorIndex >= 0) {
    rebuildMenuEntries();  // need menu count to clamp; rebuild is idempotent
    const int combinedSize = static_cast<int>(recentBooks.size() + menuEntries.size());
    if (combinedSize > 0) {
      selectorIndex = std::min(focusSelectorIndex, combinedSize - 1);
    }
  }
  focusSelectorIndex = -1;

  // Trigger first update
  menuEntriesDirty = true;
  requestUpdate();
}

void HomeActivity::onExit() {
  // The cover-loading burst is over; release the one book's metadata the memo still holds.
  Epub::clearCoverMetadataMemo();
  Activity::onExit();
  freeCoverBuffer();
  recentsHold.reset();
  UITheme::getInstance().getMutableTheme().invalidateFrameCache();
  // Never hand the next activity a degraded display: if we exit mid-load (e.g. the
  // user opened a book before covers finished), put the secondary framebuffer back.
  // The reader will release it again itself if it needs the headroom.
  // onExit runs under the RenderLock held by ActivityManager::exitActivity, so we must
  // NOT take another (non-recursive → self-deadlock); pass callerHoldsRenderLock=true.
  restoreSecondaryBuffer(/*callerHoldsRenderLock=*/true);
}

bool HomeActivity::storeCoverBuffer() {
  if (coverRectW <= 0 || coverRectH <= 0) return false;
  freeCoverBuffer();
  const size_t needed = renderer.getRegionByteSize(coverRectX, coverRectY, coverRectW, coverRectH);
  if (needed == 0) return false;
  coverBuffer = static_cast<uint8_t*>(malloc(needed));
  if (!coverBuffer) {
    LOG_ERR("HOME", "OOM: cover buffer (%u bytes)", (unsigned)needed);
    return false;
  }
  coverBufferSize = needed;
  if (!renderer.copyRegionToBuffer(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize)) {
    free(coverBuffer);
    coverBuffer = nullptr;
    coverBufferSize = 0;
    return false;
  }
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer || coverRectW <= 0 || coverRectH <= 0) return false;
  return renderer.copyBufferToRegion(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize);
}

void HomeActivity::freeCoverBuffer() {
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
  coverBufferSize = 0;
  coverBufferStored = false;
}

void HomeActivity::loop() {
  if (menuEntriesDirty) {
    rebuildMenuEntries();
  }

  // Ahead of the button handling: a tap that opens a book or a menu entry replaces or stacks on
  // this activity, and running the rest against a screen that is going away serves nobody.
  if (handleHomeTouch()) return;

  const bool isCarousel = (GUI.getHomeNavigation() == HomeNavigation::Carousel);

  // A press may be in EITHER place: hasPendingInput() reads the live sampler accumulator
  // (presses arriving this tick), while wasAnyPressed()/wasAnyReleased() read the snapshot
  // that gpio.update() already drained the accumulator INTO at the top of the main loop —
  // BEFORE this loop() runs. Gating cover loading on hasPendingInput() alone therefore
  // starves the snapshot edge: the press sits unconsumed in snapPressed_ while the gate
  // keeps diverting to loadRecentCovers() every tick, and the next update() overwrites it.
  // Defer cover loading whenever input is waiting in either place so the handlers below run.
  const bool inputWaiting =
      mappedInput.hasPendingInput() || mappedInput.wasAnyPressed() || mappedInput.wasAnyReleased();

  if (isCarousel) {
    if (firstRenderDone && !recentsLoaded && !recentsLoading && !inputWaiting) {
      loadRecentCovers(UITheme::getInstance().getMetrics().homeCoverHeight);
      return;
    }

    const int bookCount = static_cast<int>(recentBooks.size());
    const int menuItemCount = static_cast<int>(menuEntries.size());
    const bool inCarouselRow = (selectorIndex < bookCount);
    const int menuIdx = inCarouselRow ? 0 : (selectorIndex - bookCount);

    // Up and Down both toggle between the carousel row and the menu row. They are
    // handled together, and as a single else-if chain with Left/Right, so that AT MOST
    // ONE direction acts per tick. Two row-toggle edges landing in the same drained
    // input batch (easy when a slow cover-loading tick lets the sampler accumulate
    // several edges) would otherwise both run against the stale `inCarouselRow` above:
    // the first sets selectorIndex = bookCount, the second then captures THAT into
    // lastCarouselBookIndex (= bookCount, an invalid carousel index), permanently
    // stranding the selector in the menu row — Left/Right still move the icon highlight
    // but Up/Down can never return to the carousel. One-direction-per-tick prevents it.
    const bool rowToggle = mappedInput.wasLogicalPressed(MappedInputManager::Direction::Up) ||
                           mappedInput.wasLogicalPressed(MappedInputManager::Direction::Down);

    // A horizontal swipe scrolls the carousel, which is the gesture the shape of this screen
    // invites: a strip of covers with one in front and its neighbours peeking out either side.
    //
    // Direction follows the reader's own mapping (ReaderUtils::detectTouchPageTurn) rather than
    // being invented here -- a swipe that travels right-to-left brings the NEXT item, the way
    // content follows the finger. Reusing the mapping matters because the two screens sit one
    // gesture apart: swiping to the next book and then swiping to the next page must not mean
    // opposite things.
    //
    // Folded into the same else-if chain as the buttons, not handled separately, so it inherits
    // the one-action-per-tick rule that chain exists for. Two directions resolved in one drained
    // input batch is exactly what once stranded the selector in the menu row.
    //
    // GestureEventManager has already run in main.cpp and consumes the contact when the user has
    // BOUND that swipe to an action, so a bound gesture still wins; only an unbound swipe -- the
    // default -- reaches this. wasSwipe() is silent on non-touch boards and while Touch
    // Navigation is off, so neither needs a guard of its own.
    const auto swipe = mappedInput.wasSwipe();
    const bool swipeNext = swipe == MappedInputManager::SwipeDir::Left;
    const bool swipePrev = swipe == MappedInputManager::SwipeDir::Right;

    if (mappedInput.wasLogicalPressed(MappedInputManager::Direction::Right) || swipeNext) {
      if (inCarouselRow && bookCount > 0)
        selectorIndex = (selectorIndex + 1) % bookCount;
      else if (!inCarouselRow)
        selectorIndex = bookCount + (menuIdx + 1) % menuItemCount;
      requestUpdate();
    } else if (mappedInput.wasLogicalPressed(MappedInputManager::Direction::Left) || swipePrev) {
      if (inCarouselRow && bookCount > 0)
        selectorIndex = (selectorIndex + bookCount - 1) % bookCount;
      else if (!inCarouselRow)
        selectorIndex = bookCount + (menuIdx + menuItemCount - 1) % menuItemCount;
      requestUpdate();
    } else if (rowToggle) {
      if (inCarouselRow) {
        lastCarouselBookIndex = selectorIndex;  // selectorIndex < bookCount here — always valid
        selectorIndex = bookCount;              // jump to first menu entry
      } else {
        // Restore the remembered carousel slot, clamped in case it was never set or
        // the recents list shrank since it was recorded.
        selectorIndex = (bookCount > 0) ? std::min(std::max(lastCarouselBookIndex, 0), bookCount - 1) : 0;
      }
      requestUpdate();
    }
  } else {
    const int totalItems = static_cast<int>(recentBooks.size() + menuEntries.size());

    if (firstRenderDone && !recentsLoaded && !recentsLoading && !inputWaiting) {
      const auto& metrics = UITheme::getInstance().getMetrics();
      const Rect contentRect = UITheme::getContentRect(renderer, true, false);
      const HomeScreenLayout layout =
          computeHomeScreenLayout(metrics, contentRect.height, static_cast<int>(menuEntries.size()));
      loadRecentCovers(getHomeCoverRenderHeight(layout));
      return;
    }

    buttonNavigator.onNext([this, totalItems] {
      selectorIndex = ButtonNavigator::nextIndex(selectorIndex, totalItems);
      requestUpdate();
    });

    buttonNavigator.onPrevious([this, totalItems] {
      selectorIndex = ButtonNavigator::previousIndex(selectorIndex, totalItems);
      requestUpdate();
    });
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const int recentsCount = static_cast<int>(recentBooks.size());
    if (selectorIndex < recentsCount) {
      onSelectBook(recentBooks[selectorIndex].path);
    } else {
      const int menuIdx = selectorIndex - recentsCount;
      if (menuIdx < static_cast<int>(menuEntries.size())) {
        dispatchMenuAction(menuEntries[menuIdx].action);
      }
    }
  }
}

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);

  if (menuEntriesDirty) {
    rebuildMenuEntries();
  }

  const int menuCount = static_cast<int>(menuEntries.size());
  const bool isCarousel = (GUI.getHomeNavigation() == HomeNavigation::Carousel);

  // Fast path: theme owns its own pre-rendered frame cache
  if (isCarousel) {
    // The carousel scrolls along logical Left/Right and the row toggle sits on logical Up/Down, so
    // in landscape the front strip carries the toggle instead — mapHints puts the matching arrows
    // there. (Only the front strip is drawn; the side buttons are unlabelled on the home screen.)
    const auto carouselLabels =
        mappedInput.mapHints("", tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT), tr(STR_DIR_UP), tr(STR_DIR_DOWN))
            .front;
    const bool handled = GUI.tryFastHomeRender(
        renderer, recentBooks, selectorIndex, menuCount,
        [this](int index) { return std::string(I18N.get(menuEntries[index].label)); },
        [this](int index) { return menuEntries[index].icon; }, carouselLabels.btn1, carouselLabels.btn2,
        carouselLabels.btn3, carouselLabels.btn4);
    if (handled) {
      // Opens the cover-loading gate in loop(); see the note at the end of render() for
      // why this deliberately does NOT also request a second render.
      firstRenderDone = true;
      return;
    }
    // Fast path failed (e.g. frame cache malloc failed). Force cover re-render
    // so the fallback drawRecentBookCover below doesn't skip based on stale state.
    coverRendered = false;
    coverBufferStored = false;
  }

  renderer.clearScreen();
  bool bufferRestored = coverBufferStored && restoreCoverBuffer();

  GUI.drawHeader(renderer, Rect{contentRect.x, metrics.topPadding, contentRect.width, metrics.homeTopPadding}, nullptr);

  const int totalItems = static_cast<int>(recentBooks.size() + menuEntries.size());
  if (selectorIndex >= totalItems) {
    selectorIndex = std::max(0, totalItems - 1);
  }

  const HomeScreenLayout layout = computeHomeScreenLayout(metrics, contentRect.height, menuCount);

  coverRectX = contentRect.x;
  coverRectY = metrics.homeTopPadding;
  coverRectW = contentRect.width;
  coverRectH = layout.recentTileHeight;

  GUI.drawRecentBookCover(renderer,
                          Rect{contentRect.x, metrics.homeTopPadding, contentRect.width, layout.recentTileHeight},
                          recentBooks, selectorIndex, coverRendered, coverBufferStored, bufferRestored,
                          std::bind(&HomeActivity::storeCoverBuffer, this));

  GUI.drawButtonMenu(
      renderer,
      Rect{contentRect.x, metrics.homeTopPadding + layout.recentTileHeight + layout.recentToMenuGap, contentRect.width,
           layout.menuHeight},
      menuCount, selectorIndex - static_cast<int>(recentBooks.size()),
      [this](int index) { return std::string(I18N.get(menuEntries[index].label)); },
      [this](int index) { return menuEntries[index].icon; });

  const auto labels = isCarousel ? mappedInput
                                       .mapHints("", tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT),
                                                 tr(STR_DIR_UP), tr(STR_DIR_DOWN))
                                       .front
                                 : mappedInput.mapLabels("", tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();

  // Opens the cover-loading gate in loop() (which requires firstRenderDone, so that covers are
  // read off SD only after something is already on screen).
  //
  // No requestUpdate() here. It used to post a second full render unconditionally, before
  // anything knew whether the cover pass would change a pixel; the render task serialises
  // behind that pass on the RenderLock, so it landed after it and repainted the same screen.
  // On a device whose thumbs are all cached -- the steady state -- that was a whole extra panel
  // update per Home entry for nothing (measured: 643 ms on the 960x540 S3, on top of the 1573 ms
  // first paint).
  //
  // Nothing is lost by dropping it:
  //   - a cover that IS decoded already repaints itself; every path in loadRecentCovers() that
  //     writes a BMP ends in yieldAfterDecode(), which calls requestUpdate(). The remaining early
  //     returns are all "produced nothing, retry later" and need no repaint.
  //   - cover loading still starts: ActivityManager calls loop() every tick regardless of whether
  //     an update is pending, and the gate lives in loop().
  //   - the _redBaselineAuthoritative one-shot armed by reallocSecondaryBuffer() is state, not a
  //     timer: it survives until whatever paints next, which then diffs against the controller's
  //     retained RED plane exactly as it would have here.
  firstRenderDone = true;
}

// A tap on a cover or a menu entry. Both sets of targets were published by whichever theme
// painted them, so this knows nothing about where a cover or a row physically sits -- which is
// the point, given four themes that lay the home screen out four different ways (one cover,
// three covers, a carousel, and a menu that is a vertical row list in two themes and a
// horizontal icon strip in the carousel).
//
// Apply the same configurable selection/activation policy as other selectable touch surfaces.
// This includes the carousel's horizontal icon row as well as covers and vertical menu rows.
bool HomeActivity::handleHomeTouch() {
#if !CP_TOUCH_UI
  // The TapTargets stubs report no targets and hit-test to -1 on a board with no digitiser, so
  // none of this can run. See MappedInputManager::listTouch for why it is returned rather than
  // left to the optimiser.
  return false;
#else
  if (!mappedInput.hasTouch()) return false;
  if (!TapTargets::homeCovers().hasTargets() && !TapTargets::homeMenu().hasTargets()) return false;

  const int recentsCount = static_cast<int>(recentBooks.size());
  const int menuCount = static_cast<int>(menuEntries.size());

  // Covers first: in the carousel theme the side covers slide behind the centre one, and the
  // recorder orders them so the visible one wins. The two sets never overlap each other.
  // Both are bounds-checked against the CURRENT data, not the data the render saw: the targets
  // are published on the render task and recentBooks/menuEntries can change under a tap.
  int selector = -1;
  bool isBook = false;
  const auto resolve = [&](const int x, const int y) {
    const int book = TapTargets::homeCovers().hitTest(x, y);
    if (book >= 0 && book < recentsCount) {
      selector = book;
      isBook = true;
      return true;
    }
    const int menu = TapTargets::homeMenu().hitTest(x, y);
    if (menu >= 0 && menu < menuCount) {
      selector = recentsCount + menu;
      isBook = false;
      return true;
    }
    return false;
  };

  int x = 0;
  int y = 0;
  // Claimed but not acted on: moving the selection mid-contact would defeat the two-step below.
  if (mappedInput.wasScreenTouchDown(x, y) && resolve(x, y)) return true;
  if (!mappedInput.wasScreenTapped(x, y) || !resolve(x, y)) return false;

  // The whole selector is one range (covers then menu), so the comparison happens in that frame
  // and a tap on the menu correctly counts as "new" while a cover is selected, and vice versa.
  const auto tapResult = listTapActivation.applyPreference(
      selector, ListRowTap::apply(selector, recentsCount + menuCount, selectorIndex),
      SETTINGS.touchListActivation == CrossPointSettings::TOUCH_LIST_ACTIVATE_IMMEDIATELY);
  switch (tapResult) {
    case ListRowTap::Result::Rejected:
      return true;
    case ListRowTap::Result::Selected:
      requestUpdate();
      return true;
    case ListRowTap::Result::Activate:
      if (isBook) {
        onSelectBook(recentBooks[selector].path);
      } else {
        dispatchMenuAction(menuEntries[selector - recentsCount].action);
      }
      return true;
  }
  return true;
#endif  // CP_TOUCH_UI
}

void HomeActivity::onSelectBook(const std::string& path) {
  // Arm a clean HALF baseline for the reader's first page. It diffs against whatever RED RAM holds
  // — this home frame — and a FAST diff of a dramatic home->page change leaves the home screen
  // ghosting through. The reader's onEnter expects the launching activity to arm this (FileBrowser
  // does via enforceExitFullRefresh; Home didn't, so books opened from Home ghosted on entry).
  renderer.setNextDisplayRefreshMode(HalDisplay::HALF_REFRESH);
  ReturnHint hint;
  hint.target = ReturnTo::Home;
  hint.selectName = path;  // used to re-focus the book in the recents strip after return
  activityManager.replaceWithReader(path, std::move(hint));
}

void HomeActivity::dispatchMenuAction(HomeMenuAction action) {
  // Record where the menu entry was focused so that when the launched activity exits
  // (via returnFromChild() or an empty-stack finish()), we come back to the same row.
  // Also carries through More, whose goTo*() leaves the hint in place for what it opens.
  ReturnHint hint;
  hint.target = ReturnTo::Home;
  hint.selectIndex = selectorIndex;
  activityManager.setReturnHint(std::move(hint));

  activityManager.goToHomeMenuAction(action);
}
