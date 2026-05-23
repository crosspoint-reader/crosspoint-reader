#ifndef DEBUG_MEMORY_CONSUMPTION
#define DEBUG_MEMORY_CONSUMPTION 0
#endif

#include "EpubReaderActivity.h"

#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <optional>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderFootnotesActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "EpubReaderPrintedPageInputActivity.h"
#include "EpubRenderBenchmarkActivity.h"
#include "FinishedBookActivity.h"
#include "GlobalBookmarkIndex.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderDocumentId.h"
#include "MappedInputManager.h"
#include "QrDisplayActivity.h"
#include "ReaderActivity.h"
#include "ReaderUtils.h"
#include "ReadingSessionTracker.h"
#include "RecentBooksStore.h"
#include "SdCardFontGlobals.h"
#include "StarredPagesActivity.h"
#include "activities/settings/ReadingStatsBookDetailActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ScreenshotUtil.h"

namespace {
// pagesPerRefresh now comes from SETTINGS.getRefreshFrequency()
constexpr unsigned long skipChapterMs = 700;

// Parse a printed-page label as a non-negative integer. Returns nullopt for empty strings,
// strings with non-digit characters (e.g. roman "iv"), and overflow. Used both to gate the
// "go to printed page" menu item and to compute min/max for the numeric input.
std::optional<int> parsePrintedPageLabel(const std::string& label) {
  if (label.empty()) return std::nullopt;
  int value = 0;
  for (char c : label) {
    if (c < '0' || c > '9') return std::nullopt;
    value = value * 10 + (c - '0');
    if (value > 999999) return std::nullopt;  // sanity
  }
  return value;
}
// pages per minute, first item is 1 to prevent division by zero if accessed
constexpr int PAGE_TURN_LABELS[] = {1, 1, 3, 6, 12};

// Silent next-chapter indexing should only run when heap is healthy enough,
// otherwise it tends to produce heavily truncated fallback caches.
#ifndef CP_SILENT_INDEX_MIN_FREE_HEAP_BYTES
#define CP_SILENT_INDEX_MIN_FREE_HEAP_BYTES (64 * 1024)
#endif

#ifndef CP_SILENT_INDEX_MIN_CONTIG_HEAP_BYTES
#define CP_SILENT_INDEX_MIN_CONTIG_HEAP_BYTES (24 * 1024)
#endif

constexpr uint32_t SILENT_INDEX_MIN_FREE_HEAP_BYTES = CP_SILENT_INDEX_MIN_FREE_HEAP_BYTES;
constexpr uint32_t SILENT_INDEX_MIN_CONTIG_HEAP_BYTES = CP_SILENT_INDEX_MIN_CONTIG_HEAP_BYTES;

// Hysteresis for auto AA recovery after low-memory BW snapshot failures.
// Override at build time via -D flags, for example:
//   -DCP_AA_RECOVERY_FREE_HEAP_BYTES=30720
//   -DCP_AA_RECOVERY_CONTIG_HEAP_BYTES=12288
#ifndef CP_AA_RECOVERY_FREE_HEAP_BYTES
#define CP_AA_RECOVERY_FREE_HEAP_BYTES (28 * 1024)
#endif

#ifndef CP_AA_RECOVERY_CONTIG_HEAP_BYTES
#define CP_AA_RECOVERY_CONTIG_HEAP_BYTES (12 * 1024)
#endif

constexpr uint32_t AA_RECOVERY_FREE_HEAP_BYTES = CP_AA_RECOVERY_FREE_HEAP_BYTES;
constexpr uint32_t AA_RECOVERY_CONTIG_HEAP_BYTES = CP_AA_RECOVERY_CONTIG_HEAP_BYTES;

// Snapshotting BW buffer for grayscale restore can fragment heap heavily on tight
// pages. Skip attempting snapshot altogether below this safety window.
#ifndef CP_BW_SNAPSHOT_MIN_FREE_HEAP_BYTES
#define CP_BW_SNAPSHOT_MIN_FREE_HEAP_BYTES (64 * 1024)
#endif

#ifndef CP_BW_SNAPSHOT_MIN_CONTIG_HEAP_BYTES
#define CP_BW_SNAPSHOT_MIN_CONTIG_HEAP_BYTES (24 * 1024)
#endif

constexpr uint32_t BW_SNAPSHOT_MIN_FREE_HEAP_BYTES = CP_BW_SNAPSHOT_MIN_FREE_HEAP_BYTES;
constexpr uint32_t BW_SNAPSHOT_MIN_CONTIG_HEAP_BYTES = CP_BW_SNAPSHOT_MIN_CONTIG_HEAP_BYTES;
constexpr uint8_t TRUNCATED_SECTION_HINT_RENDER_COUNT = 2;
constexpr const char* TRUNCATED_SECTION_HINT_LINE_1 = "Chapter may be truncated (low memory).";
constexpr const char* TRUNCATED_SECTION_HINT_LINE_2 = "Try: No embedded style | No images | AA Off";

#if DEBUG_MEMORY_CONSUMPTION
void logReaderMemSnapshot(const char* stage) {
  const uint32_t freeHeap = esp_get_free_heap_size();
  const uint32_t contigHeap = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
  LOG_DBG("ERS", "Reader mem[%s]: free=%lu contig=%lu", stage, freeHeap, contigHeap);
}
#else
inline void logReaderMemSnapshot(const char*) {}
#endif

// Integrity bisector. Logs at every probe site (unconditional, not gated) and
// fires an ERR when integrity transitions from ok -> fail so we can pinpoint
// which render phase corrupts the heap. Free/contig included so we can see if
// the corruption coincides with a specific allocation pattern. Calling
// heap_caps_check_integrity_all is ~O(blocks) — not free but fine at phase
// boundaries during onEnter / first render.
void logIntegrityProbe(const char* stage) {
  static bool sLastOk = true;
  const bool ok = heap_caps_check_integrity_all(true);
  const uint32_t freeHeap = esp_get_free_heap_size();
  const uint32_t contigHeap = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
  if (ok != sLastOk) {
    if (ok) {
      LOG_DBG("INTG", "[%s] integrity recovered (free=%lu contig=%lu)", stage, freeHeap, contigHeap);
    } else {
      LOG_ERR("INTG", "[%s] integrity FAIL — corruption introduced here (free=%lu contig=%lu)", stage, freeHeap,
              contigHeap);
    }
    sLastOk = ok;
  } else {
    LOG_DBG("INTG", "[%s] %s free=%lu contig=%lu", stage, ok ? "ok" : "fail", freeHeap, contigHeap);
  }
}

// Tiled grayscale: render each plane band-by-band into a small scratch and
// stream straight to the controller, leaving the BW framebuffer intact so no
// storeBwBuffer / restoreBwBuffer is needed. Controller RAM is re-synced from
// the live framebuffer afterward via cleanupGrayscaleWithFrameBuffer().
//
// Returns true when the strip path ran end-to-end (controller now holds the AA
// planes and the live BW frame is clean). Returns false when the controller
// doesn't support strip grayscale OR the scratch allocation fails — caller
// should fall back to the legacy storeBwBufferRect path.
//
// The page is re-rendered ceil(panelHeight/STRIP_ROWS) times per plane, but
// renderCharImpl culls out-of-band glyphs before bitmap decode so the cost
// stays close to one render. Only renderTextOnly() is called here, matching the
// legacy AA pass — images and HRs do not participate in grayscale.
bool runTiledGrayscalePass(GfxRenderer& renderer, const Page& page, int fontId, int marginLeft, int contentTop,
                           bool fastAA) {
  if (!renderer.supportsStripGrayscale()) return false;

  // Push the SETTINGS toggle into the SDK before the AA refresh. No-op on X4;
  // on X3 picks between OEM _gc (slow/accurate) and community _grayscale
  // (fast/darker mid-tones). Re-applied per render so a settings change takes
  // effect on the next page flip without rebooting.
  renderer.setFastGrayscaleLut(fastAA);

  // Strip height trades scratch size for the number of re-renders. Each render
  // pays layout + glyph-cull overhead even when bitmap decode is skipped, so
  // fewer/bigger bands win as long as the scratch fits. 240 rows × ~100 bytes
  // ≈ ~24 KB — still well below the legacy partial-snapshot footprint while
  // cutting X3 (480 px) to 2 bands/plane and X4 (800 px) to 4 bands/plane.
  constexpr int STRIP_ROWS = 240;
  const int gh = renderer.getDisplayHeight();
  const int gwBytes = renderer.getDisplayWidthBytes();

  auto scratch = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[static_cast<size_t>(gwBytes) * STRIP_ROWS]);
  if (!scratch) {
    LOG_INF("ERS", "Tiled grayscale: scratch alloc failed (%d bytes); falling back to legacy path",
            gwBytes * STRIP_ROWS);
    return false;
  }

  auto renderPlane = [&](GfxRenderer::RenderMode mode, bool lsbPlane) {
    renderer.setRenderMode(mode);
    for (int y = 0; y < gh; y += STRIP_ROWS) {
      const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
      renderer.beginStripTarget(scratch.get(), y, rows);
      renderer.clearScreen(0x00);
      page.renderTextOnly(renderer, fontId, marginLeft, contentTop);
      renderer.endStripTarget();
      renderer.writeGrayscalePlaneStrip(lsbPlane, scratch.get(), y, rows);
    }
  };

  logIntegrityProbe("tiledGray_after_scratchAlloc");
  renderPlane(GfxRenderer::GRAYSCALE_LSB, true);
  logIntegrityProbe("tiledGray_after_lsbPlane");
  renderPlane(GfxRenderer::GRAYSCALE_MSB, false);
  logIntegrityProbe("tiledGray_after_msbPlane");

  renderer.setRenderMode(GfxRenderer::BW);
  renderer.displayGrayBuffer();
  logIntegrityProbe("tiledGray_after_displayGrayBuffer");

  // BW framebuffer is intact; re-sync controller RAM for the next differential
  // page turn directly from it.
  renderer.cleanupGrayscaleWithFrameBuffer();
  logIntegrityProbe("tiledGray_after_cleanup");
  return true;
}

// Computes the [0..100] EPUB progress percent. Returns 0 when pageCount is unknown (sync/bookmark
// pre-render writes), in which case the next saveProgress() will overwrite progress.bin with the
// real value before the user can leave the reader.
uint8_t epubProgressPercentByte(const Epub& epub, const int spineIndex, const int currentPage, const int pageCount) {
  if (pageCount <= 0) {
    return 0;
  }
  const float chapterProgress = static_cast<float>(currentPage) / static_cast<float>(pageCount);
  return ReaderUtils::fractionProgressPercentByte(epub.calculateProgress(spineIndex, chapterProgress));
}

// Writes the canonical EPUB progress.bin layout: spine(2) + page(2) + pageCount(2) + percent(1).
// Used by the per-page saveProgress() and by transient writers (sync restore, bookmark jump) so
// the on-disk format stays consistent regardless of caller.
bool writeReaderProgressCache(const std::string& cachePath, const int spineIndex, const int currentPage,
                              const int pageCount, const uint8_t percent) {
  FsFile f;
  if (!Storage.openFileForWrite("ERS", cachePath + "/progress.bin", f)) {
    LOG_ERR("ERS", "Failed to open progress cache: %s", cachePath.c_str());
    return false;
  }

  uint8_t data[7];
  data[0] = spineIndex & 0xFF;
  data[1] = (spineIndex >> 8) & 0xFF;
  data[2] = currentPage & 0xFF;
  data[3] = (currentPage >> 8) & 0xFF;
  data[4] = pageCount & 0xFF;
  data[5] = (pageCount >> 8) & 0xFF;
  data[6] = percent;
  f.write(data, 7);
  f.close();
  return true;
}

int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

const char* orientationToString(const GfxRenderer::Orientation orientation) {
  switch (orientation) {
    case GfxRenderer::Portrait:
      return "Portrait";
    case GfxRenderer::LandscapeClockwise:
      return "Landscape CW";
    case GfxRenderer::PortraitInverted:
      return "Portrait Inverted";
    case GfxRenderer::LandscapeCounterClockwise:
      return "Landscape CCW";
  }
  return "Unknown";
}

int getImageOnlyPageYOffset(const Page& page, const int viewportHeight) {
  if (viewportHeight <= 0 || page.elements.empty()) {
    return 0;
  }

  const bool imageOnlyPage = std::all_of(
      page.elements.begin(), page.elements.end(),
      [](const std::shared_ptr<PageElement>& element) { return element && element->getTag() == TAG_PageImage; });
  if (!imageOnlyPage) {
    return 0;
  }

  int16_t imgX, imgY, imgW, imgH;
  if (!page.getImageBoundingBox(imgX, imgY, imgW, imgH) || imgH >= viewportHeight) {
    return 0;
  }

  const int centeredTop = (viewportHeight - imgH) / 2;
  return std::max(0, centeredTop - static_cast<int>(imgY));
}

}  // namespace

void EpubReaderActivity::onEnter() {
  Activity::onEnter();
  logReaderMemSnapshot("onEnter_begin");
  logIntegrityProbe("onEnter_begin");

  // Drop any input events that arrived from the activity that launched us (e.g. a wake-up power
  // button hold) before they reach detectPageTurn() — see ReaderUtils::InputDrainGuard.
  inputDrainGuard.arm();

  if (!epub) {
    return;
  }

  // Configure screen orientation based on settings
  // NOTE: This affects layout math and must be applied before any render calls.
  {
    RenderLock lock(*this);
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  }
  logReaderMemSnapshot("onEnter_after_orientation");

  epub->setupCacheDir();
  logReaderMemSnapshot("onEnter_after_setupCacheDir");

  // Load the persistent baseline (progress.bin) first. Pending session state
  // (sync result, bookmark jump) is then overlaid on top — this is the only order
  // that lets a Kind::Paragraph / Kind::ListItem navTarget set by applyPendingSyncSession
  // survive into render(). The previous order (apply then load) clobbered the LUT
  // target with Kind::Page from progress.bin, which is why XPath-precision sync
  // silently degraded to the rough page estimate.
  FsFile f;
  if (Storage.openFileForRead("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    int dataSize = f.read(data, 6);
    if (dataSize == 4 || dataSize == 6) {
      currentSpineIndex = data[0] + (data[1] << 8);
      navTarget = NavigationTarget::makePage(data[2] + (data[3] << 8));
      navTarget.cachedSpineIdx = currentSpineIndex;
      LOG_DBG("ERS", "Loaded cache: %d, %d", currentSpineIndex, navTarget.page);
    }
    if (dataSize == 6) {
      navTarget.cachedPageCount = data[4] + (data[5] << 8);
    }
    f.close();
  }
  // We may want a better condition to detect if we are opening for the first time.
  // This will trigger if the book is re-opened at Chapter 0.
  if (currentSpineIndex < 0 || currentSpineIndex >= epub->getSpineItemsCount()) {
    LOG_ERR("ERS", "Invalid saved spine index %d (valid 0..%d), resetting to start", currentSpineIndex,
            epub->getSpineItemsCount() > 0 ? epub->getSpineItemsCount() - 1 : 0);
    currentSpineIndex = 0;
    navTarget = NavigationTarget::makePage(0);
  }

  applyPendingSyncSession();
  applyPendingBookmarkJump();
  logReaderMemSnapshot("onEnter_after_pending_sync");

  if (currentSpineIndex == 0) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      LOG_DBG("ERS", "Opened for first time, navigating to text reference at index %d", textSpineIndex);
    }
  }
  logReaderMemSnapshot("onEnter_after_progress_load");

  // Load bookmarks for this book
  bookmarkStore.load(epub->getCachePath());
  logReaderMemSnapshot("onEnter_after_bookmarks_loaded");

  // Save current epub as last opened epub and add to recent books
  APP_STATE.openEpubPath = epub->getPath();
  APP_STATE.saveToFile();
  std::string series = epub->getSeries();
  if (!series.empty() && !epub->getSeriesIndex().empty()) {
    series += " #" + epub->getSeriesIndex();
  }
  const std::string epubSidecar = ReaderActivity::sidecarCoverPath(epub->getPath());
  const std::string epubCover = epubSidecar.empty() ? epub->getThumbBmpPath() : epubSidecar;
  RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), series, epubCover);
  const RecentBook currentBook = RECENT_BOOKS.getBookByPath(epub->getPath());
  bookEmbeddedStyleOverride = currentBook.embeddedStyleOverride;
  bookImageRenderingOverride = currentBook.imageRenderingOverride;
  bookFontFamilyOverride = currentBook.fontFamilyOverride;
  bookSdFontFamilyOverride = currentBook.sdFontFamilyOverride;
  bookFontSizeOverride = currentBook.fontSizeOverride;
  bookBionicReadingOverride = (currentBook.bionicReadingOverride >= 0)
                                  ? static_cast<bool>(currentBook.bionicReadingOverride)
                                  : static_cast<bool>(SETTINGS.bionicReading);
  bookParagraphAlignmentOverride = currentBook.paragraphAlignmentOverride;
  logReaderMemSnapshot("onEnter_after_recent_books");

  // Start a reading-stats session. We use the cheap filename-based hash here:
  // computing the content hash would re-read the file on every reader open,
  // and a renamed book getting a new stats entry is acceptable — it'll still
  // accumulate going forward.
  globalReadingSessionTracker().begin(KOReaderDocumentId::calculateFromFilename(epub->getPath()), epub->getTitle(),
                                      epub->getAuthor());

  // Trigger first update
  logReaderMemSnapshot("onEnter_before_request_update");
  requestUpdate();
  logReaderMemSnapshot("onEnter_ready");
}

void EpubReaderActivity::onExit() {
  Activity::onExit();
  logReaderMemSnapshot("onExit_before_release");

  // Flush the reading-stats session before tearing down the epub: end() needs
  // no live epub reference and persists the JSON. Sleep paths that bypass
  // onExit() still end up here on resume because the activity is recreated.
  globalReadingSessionTracker().end();
  // If a pre-render left the next page in the frame buffer, redraw the current page so the
  // next activity (notably SleepActivity's OVERLAY mode) sees what the user was looking at.
  // Must run before section.reset() and the orientation reset below.
  restoreCurrentPageToBufferIfPreRendered();

  // Save bookmarks before exit
  bookmarkStore.save();
  if (epub) {
    GLOBAL_BOOKMARKS.syncFromStore(bookmarkStore, epub->getPath(), epub->getCachePath(), epub->getTitle(), false);
  }

  // Reset orientation back to portrait for the rest of the UI
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  section.reset();
  UITheme::getInstance().getMutableTheme().onBookWillClose(epub ? epub->getPath() : "", epub.get(), nullptr, nullptr);
  epub.reset();
  currentPageFootnotes.clear();
  currentPageFootnotes.shrink_to_fit();
  logReaderMemSnapshot("onExit_after_release");
}

void EpubReaderActivity::loop() {
  if (!epub) {
    // Should never happen
    finish();
    return;
  }

  if (pendingProgressSave.pending.load(std::memory_order_acquire)) {
    pendingProgressSave.pending.store(false, std::memory_order_relaxed);
    saveProgress(pendingProgressSave.spineIndex, pendingProgressSave.page, pendingProgressSave.pageCount);
  }

  if (inputDrainGuard.shouldDrain(mappedInput)) {
    buttonEvents.drain();
    return;
  }

  if (automaticPageTurnActive) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      buttonEvents.drain();
      stopAutomaticPageTurn();
      // updates chapter title space to indicate page turn disabled
      requestUpdate();
      return;
    }

    if (!section) {
      requestUpdate();
      return;
    }

    // Skips page turn if renderingMutex is busy
    if (RenderLock::peek()) {
      lastPageTurnTime = millis();
      return;
    }

    if ((millis() - lastPageTurnTime) >= pageTurnDuration) {
      pageTurn(true);
      return;
    }
  }

  bool delayedPrevTurn = false;
  bool delayedNextTurn = false;
  using BA = CrossPointSettings::BUTTON_ACTION;

  ButtonEventManager::ButtonEvent ev;
  while (buttonEvents.consumeEvent(ev)) {
    if (ev.button == MappedInputManager::Button::Confirm) {
      if (ev.type == ButtonEventManager::PressType::Long && KOREADER_STORE.hasCredentials()) {
        launchKOReaderSync(SyncLaunchMode::COMPARE);
        return;
      }
      if (ev.type == ButtonEventManager::PressType::Short) {
        if (pageHasPlaceholders) {
          forceLoadLargeImages = true;
          pageHasPlaceholders = false;
          requestUpdate();
          return;
        }
        openReaderMenu();
        return;
      }
    }

    if (ev.button == MappedInputManager::Button::Back) {
      if (ev.type == ButtonEventManager::PressType::Long) {
        ReaderUtils::enforceExitFullRefresh(renderer);
        if (tryAutoPushOnClose()) return;
        onGoHome();
        return;
      }
      if (ev.type == ButtonEventManager::PressType::Short) {
        if (footnoteDepth > 0) {
          restoreSavedPosition();
          return;
        }
        ReaderUtils::enforceExitFullRefresh(renderer);
        if (tryAutoPushOnClose()) return;
        finish();
        return;
      }
    }

    if (ev.type == ButtonEventManager::PressType::Short) {
      if ((ev.button == MappedInputManager::Button::PageBack && SETTINGS.btnShortPageBack == BA::BTN_DEFAULT &&
           globalButtonEvents().hasDoubleAction(MappedInputManager::Button::PageBack)) ||
          (ev.button == MappedInputManager::Button::Left && SETTINGS.btnShortLeft == BA::BTN_DEFAULT &&
           globalButtonEvents().hasDoubleAction(MappedInputManager::Button::Left))) {
        delayedPrevTurn = true;
        continue;
      }
      if ((ev.button == MappedInputManager::Button::PageForward && SETTINGS.btnShortPageForward == BA::BTN_DEFAULT &&
           globalButtonEvents().hasDoubleAction(MappedInputManager::Button::PageForward)) ||
          (ev.button == MappedInputManager::Button::Right && SETTINGS.btnShortRight == BA::BTN_DEFAULT &&
           globalButtonEvents().hasDoubleAction(MappedInputManager::Button::Right))) {
        delayedNextTurn = true;
        continue;
      }
    }
  }

  auto [prevTriggered, nextTriggered] = ReaderUtils::detectPageTurn(mappedInput);
  if (!prevTriggered && !nextTriggered) {
    if (!delayedPrevTurn && !delayedNextTurn) {
      return;
    }
    prevTriggered = delayedPrevTurn;
    nextTriggered = delayedNextTurn;
  }
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  // At end of the book, forward button opens the finished-book flow and back returns to last page
  if (currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount()) {
    if (nextTriggered) {
      const int lastSpineIndex = epub->getSpineItemsCount() - 1;
      int lastPageIndex = 0;
      int lastPageCount = 0;
      if (section && currentSpineIndex == lastSpineIndex) {
        lastPageCount = section->pageCount;
        lastPageIndex = std::max(0, section->pageCount - 1);
      }
      writeReaderProgressCache(epub->getCachePath(), lastSpineIndex, lastPageIndex, lastPageCount, 100);

      const std::string nextBookPath =
          BookFinished::findNextBookInDirectory(epub->getPath(), epub->getSeries(), epub->getSeriesIndex());
      startActivityForResult(
          std::make_unique<FinishedBookActivity>(renderer, mappedInput, epub->getPath(), nextBookPath),
          [this, nextBookPath](const ActivityResult& result) {
            if (result.isCancelled) {
              requestUpdate();
              return;
            }
            // User confirmed they're done with this book — credit a finish
            // to the in-flight session before any tear-down side effects.
            globalReadingSessionTracker().markFinished();
            const auto& menuResult = std::get<MenuResult>(result.data);
            if (menuResult.action == static_cast<int>(BookFinished::FinishedBookAction::GoHome)) {
              if (SETTINGS.moveFinishedBooksToCompleted) {
                std::string movedPath;
                BookFinished::moveFinishedBookToCompleted(epub->getPath(), movedPath);
              }
              if (SETTINGS.removeFinishedBooksFromRecents) {
                RECENT_BOOKS.removeBook(epub->getPath());
              }
              activityManager.goHome();
              return;
            }
            if (menuResult.action == static_cast<int>(BookFinished::FinishedBookAction::OpenNextBook) &&
                !nextBookPath.empty()) {
              if (SETTINGS.moveFinishedBooksToCompleted) {
                std::string movedPath;
                BookFinished::moveFinishedBookToCompleted(epub->getPath(), movedPath);
              }
              if (SETTINGS.removeFinishedBooksFromRecents) {
                RECENT_BOOKS.removeBook(epub->getPath());
              }
              activityManager.goToReader(nextBookPath);
              return;
            }
            requestUpdate();
          });
    } else {
      currentSpineIndex = epub->getSpineItemsCount() - 1;
      navTarget = NavigationTarget::makeLastPage();
      requestUpdate();
    }
    return;
  }

  // No current section, attempt to rerender the book
  if (!section) {
    requestUpdate();
    return;
  }

  if (prevTriggered) {
    pageTurn(false);
  } else {
    pageTurn(true);
  }
}

// Translate an absolute percent into a spine index plus a normalized position
// within that spine so we can jump after the section is loaded.
void EpubReaderActivity::jumpToPercent(int percent) {
  if (!epub) {
    return;
  }

  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) {
    return;
  }

  // Normalize input to 0-100 to avoid invalid jumps.
  percent = clampPercent(percent);

  // Convert percent into a byte-like absolute position across the spine sizes.
  // Use an overflow-safe computation: (bookSize / 100) * percent + (bookSize % 100) * percent / 100
  size_t targetSize =
      (bookSize / 100) * static_cast<size_t>(percent) + (bookSize % 100) * static_cast<size_t>(percent) / 100;
  if (percent >= 100) {
    // Ensure the final percent lands inside the last spine item.
    targetSize = bookSize - 1;
  }

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0) {
    return;
  }

  int targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  for (int i = 0; i < spineCount; i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      // Found the spine item containing the absolute position.
      targetSpineIndex = i;
      prevCumulative = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;
  float spineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);
  if (spineProgress < 0.0f)
    spineProgress = 0.0f;
  else if (spineProgress > 1.0f)
    spineProgress = 1.0f;

  // Reset state so render() reloads and repositions on the target spine.
  {
    RenderLock lock(*this);
    currentSpineIndex = targetSpineIndex;
    navTarget = NavigationTarget::makePercent(spineProgress);
    section.reset();
  }
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  switch (action) {
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      const int spineIdx = currentSpineIndex;
      const int tocIdx = section ? section->getTocIndexForPage(section->currentPage)
                                 : epub->getTocIndexForSpineIndex(currentSpineIndex);
      const std::string path = epub->getPath();
      startActivityForResult(
          std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub, path, spineIdx, tocIdx),
          [this](const ActivityResult& result) {
            if (result.isCancelled) return;
            RenderLock lock(*this);
            const auto& chapter = std::get<ChapterResult>(result.data);
            auto resolvedPage = (chapter.tocIndex && chapter.spineIndex == currentSpineIndex && section)
                                    ? section->getPageForTocIndex(*chapter.tocIndex)
                                    : std::nullopt;
            if (resolvedPage) {
              section->currentPage = *resolvedPage;
              forceLoadLargeImages = false;
              pageHasPlaceholders = false;
            } else {
              navTarget =
                  chapter.tocIndex ? NavigationTarget::makeTocIndex(*chapter.tocIndex) : NavigationTarget::makePage(0);
              currentSpineIndex = chapter.spineIndex;
              section.reset();
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::FOOTNOTES: {
      startActivityForResult(std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
                             [this](const ActivityResult& result) {
                               if (!result.isCancelled) {
                                 const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                                 navigateToHref(footnoteResult.href, true);
                               }
                               requestUpdate();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
      float bookProgress = 0.0f;
      if (epub && epub->getBookSize() > 0 && section && section->pageCount > 0) {
        const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
        bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
      }
      const int initialPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
      startActivityForResult(
          std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              jumpToPercent(std::get<PercentResult>(result.data).percent);
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_TO_PRINTED_PAGE: {
      if (!epub) break;
      auto entries = epub->loadPrintedPageList();
      // Compute the integer label range from parseable entries; non-integer labels are
      // ignored (the dialog is numeric-only).
      int minLabel = std::numeric_limits<int>::max();
      int maxLabel = std::numeric_limits<int>::min();
      for (const auto& entry : entries) {
        if (const auto n = parsePrintedPageLabel(entry.label)) {
          if (*n < minLabel) minLabel = *n;
          if (*n > maxLabel) maxLabel = *n;
        }
      }
      if (maxLabel < minLabel) break;  // no integer labels — shouldn't happen if menu item was shown

      // Pre-fill with the printed page the reader is currently on (or the nearest one before
      // it — rendered device pages rarely carry an anchor themselves, but they sit between
      // two printed pages, so the closest prior anchor is the "you're here" hint). Falls
      // back to the lowest integer label in the book if no prior anchor exists.
      int initialValue = minLabel;
      if (section) {
        if (const auto rawLabel =
                section->getNearestPrintedPageLabelAtOrBefore(static_cast<uint16_t>(section->currentPage))) {
          if (const auto n = parsePrintedPageLabel(*rawLabel)) {
            initialValue = *n;
          }
        }
      }

      startActivityForResult(
          std::make_unique<EpubReaderPrintedPageInputActivity>(renderer, mappedInput, initialValue, minLabel, maxLabel),
          [this, entries = std::move(entries)](const ActivityResult& result) {
            if (result.isCancelled) return;
            const auto& pick = std::get<PrintedPageResult>(result.data);
            // Resolve the typed label back to a (href, anchor) by linear scan. Entries are
            // small (typically <500 even for long books) and this fires once per user action.
            for (const auto& entry : entries) {
              const auto entryLabelValue = parsePrintedPageLabel(entry.label);
              const auto pickLabelValue = parsePrintedPageLabel(pick.label);
              if (entry.label == pick.label ||
                  (entryLabelValue && pickLabelValue && *entryLabelValue == *pickLabelValue)) {
                const int spineIdx = epub->resolveHrefToSpineIndex(entry.href);
                if (spineIdx < 0) {
                  LOG_DBG("ERS", "printed-page jump: could not resolve spine for href=%s", entry.href.c_str());
                  return;
                }
                {
                  RenderLock lock(*this);
                  currentSpineIndex = spineIdx;
                  navTarget =
                      entry.anchor.empty() ? NavigationTarget::makePage(0) : NavigationTarget::makeAnchor(entry.anchor);
                  section.reset();
                }
                requestUpdate();
                return;
              }
            }
            LOG_DBG("ERS", "printed-page jump: label '%s' not found in pagelist", pick.label.c_str());
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DISPLAY_QR: {
      if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
        auto p = section->loadPageFromSectionFile();
        if (p) {
          std::string fullText;
          for (const auto& el : p->elements) {
            if (el->getTag() == TAG_PageLine) {
              const auto& line = static_cast<const PageLine&>(*el);
              if (line.getBlock()) {
                const auto& words = line.getBlock()->getWords();
                for (const auto& w : words) {
                  if (!fullText.empty()) fullText += " ";
                  fullText += w;
                }
              }
            }
          }
          if (!fullText.empty()) {
            startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, fullText),
                                   [this](const ActivityResult& result) {});
            break;
          }
        }
      }
      // If no text or page loading failed, just close menu
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::STAR_PAGE: {
      if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
        bookmarkStore.toggle(static_cast<uint16_t>(currentSpineIndex), static_cast<uint16_t>(section->currentPage));
        requestUpdate();
      }
      break;
    }
    case EpubReaderMenuActivity::MenuAction::STARRED_PAGES: {
      startActivityForResult(
          std::make_unique<StarredPagesActivity>(renderer, mappedInput, bookmarkStore, epub),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              const auto& starred = std::get<StarredPageResult>(result.data);
              if (currentSpineIndex != starred.spineIndex || !section || section->currentPage != starred.pageNumber) {
                RenderLock lock(*this);
                currentSpineIndex = starred.spineIndex;
                navTarget = NavigationTarget::makePage(starred.pageNumber);
                section.reset();
              }
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_HOME: {
      if (tryAutoPushOnClose()) return;
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::READING_STATS: {
      // Jump to this book's detail screen using the same filename-hash docId
      // the session was opened with. The in-flight session's time isn't
      // visible here — it lands in the store only when end() runs on reader
      // exit. For a brand-new book that's never been finished a session yet
      // the screen will show "no data"; that's accurate.
      if (!epub) break;
      startActivityForResult(std::make_unique<ReadingStatsBookDetailActivity>(
                                 renderer, mappedInput, KOReaderDocumentId::calculateFromFilename(epub->getPath())),
                             [this](const ActivityResult&) { requestUpdate(); });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::MARK_AS_READ: {
      if (!epub) {
        break;
      }
      const int spineCount = epub->getSpineItemsCount();
      if (spineCount > 0) {
        const int lastSpineIndex = spineCount - 1;
        int lastPageIndex = 0;
        int lastPageCount = 0;
        if (section && currentSpineIndex == lastSpineIndex) {
          lastPageCount = section->pageCount;
          lastPageIndex = std::max(0, section->pageCount - 1);
        }
        if (lastPageCount > 0) {
          writeReaderProgressCache(epub->getCachePath(), lastSpineIndex, lastPageIndex, lastPageCount, 100);
        } else {
          writeReaderProgressCache(epub->getCachePath(), lastSpineIndex, 0, 0, 100);
        }
      }
      {
        const std::string nextBookPath =
            BookFinished::findNextBookInDirectory(epub->getPath(), epub->getSeries(), epub->getSeriesIndex());
        startActivityForResult(
            std::make_unique<FinishedBookActivity>(renderer, mappedInput, epub->getPath(), nextBookPath),
            [this, nextBookPath](const ActivityResult& result) {
              if (result.isCancelled) {
                requestUpdate();
                return;
              }
              globalReadingSessionTracker().markFinished();
              const auto& menuResult = std::get<MenuResult>(result.data);
              if (menuResult.action == static_cast<int>(BookFinished::FinishedBookAction::GoHome)) {
                if (SETTINGS.moveFinishedBooksToCompleted) {
                  std::string movedPath;
                  BookFinished::moveFinishedBookToCompleted(epub->getPath(), movedPath);
                }
                if (SETTINGS.removeFinishedBooksFromRecents) {
                  RECENT_BOOKS.removeBook(epub->getPath());
                }
                activityManager.goHome();
                return;
              }
              if (menuResult.action == static_cast<int>(BookFinished::FinishedBookAction::OpenNextBook) &&
                  !nextBookPath.empty()) {
                if (SETTINGS.moveFinishedBooksToCompleted) {
                  std::string movedPath;
                  BookFinished::moveFinishedBookToCompleted(epub->getPath(), movedPath);
                }
                if (SETTINGS.removeFinishedBooksFromRecents) {
                  RECENT_BOOKS.removeBook(epub->getPath());
                }
                activityManager.goToReader(nextBookPath);
                return;
              }
              requestUpdate();
            });
      }
      return;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      {
        RenderLock lock(*this);
        if (epub && section) {
          uint16_t backupSpine = currentSpineIndex;
          uint16_t backupPage = section->currentPage;
          uint16_t backupPageCount = section->pageCount;
          section.reset();
          epub->clearCache(true);
          epub->setupCacheDir();
          saveProgress(backupSpine, backupPage, backupPageCount);
          if (!bookmarkStore.isEmpty()) {
            bookmarkStore.markDirty();
            bookmarkStore.save();
            GLOBAL_BOOKMARKS.syncFromStore(bookmarkStore, epub->getPath(), epub->getCachePath(), epub->getTitle(),
                                           false);
          }
        }
      }
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::RENDER_BENCHMARK: {
      runRenderBenchmark();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SCREENSHOT: {
      {
        RenderLock lock(*this);
        pendingScreenshot = true;
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::PULL_REMOTE: {
      // One-tap pull path: run network preconditions and apply remote progress
      // directly instead of showing an intermediate chooser screen.
      if (KOREADER_STORE.hasCredentials()) {
        launchKOReaderSync(SyncLaunchMode::PULL_REMOTE);
      }
      break;
    }
    case EpubReaderMenuActivity::MenuAction::PUSH_LOCAL: {
      // One-tap push path: run network preconditions and upload local progress
      // directly for KOReader-like "sync now" behavior.
      if (KOREADER_STORE.hasCredentials()) {
        launchKOReaderSync(SyncLaunchMode::PUSH_LOCAL);
      }
      break;
    }
  }
}

void EpubReaderActivity::runRenderBenchmark() {
  if (!epub) {
    return;
  }

  if (!section) {
    requestUpdateAndWait();
    if (!section) {
      return;
    }
  }

  const LastRenderStats startSnapshot = lastRenderStats;
  BenchmarkAggregate aggregate;
  auto recordRender = [&aggregate](const LastRenderStats& snapshot) {
    if (!snapshot.valid) {
      return;
    }

    aggregate.renderCount++;
    aggregate.imagePageCount += snapshot.hadImages ? 1 : 0;
    aggregate.cacheRebuildCount += snapshot.cacheRebuilt ? 1 : 0;
    if (snapshot.footnoteCount > aggregate.maxFootnotes) {
      aggregate.maxFootnotes = snapshot.footnoteCount;
    }

    aggregate.totalRequestRenderMs += snapshot.requestRenderMs;
    if (aggregate.renderCount == 1 || snapshot.requestRenderMs < aggregate.minRequestRenderMs) {
      aggregate.minRequestRenderMs = snapshot.requestRenderMs;
    }
    if (snapshot.requestRenderMs > aggregate.maxRequestRenderMs) {
      aggregate.maxRequestRenderMs = snapshot.requestRenderMs;
    }

    aggregate.totalRenderMs += snapshot.phases.totalMs;
    if (aggregate.renderCount == 1 || snapshot.phases.totalMs < aggregate.minRenderMs) {
      aggregate.minRenderMs = snapshot.phases.totalMs;
    }
    if (snapshot.phases.totalMs > aggregate.maxRenderMs) {
      aggregate.maxRenderMs = snapshot.phases.totalMs;
    }

    aggregate.totalSectionLoadMs += snapshot.sectionLoadMs;
    aggregate.totalPageLoadMs += snapshot.pageLoadMs;
    aggregate.totalPhases.prewarmMs += snapshot.phases.prewarmMs;
    aggregate.totalPhases.bwRenderMs += snapshot.phases.bwRenderMs;
    aggregate.totalPhases.displayMs += snapshot.phases.displayMs;
    aggregate.totalPhases.bwStoreMs += snapshot.phases.bwStoreMs;
    aggregate.totalPhases.grayLsbMs += snapshot.phases.grayLsbMs;
    aggregate.totalPhases.grayMsbMs += snapshot.phases.grayMsbMs;
    aggregate.totalPhases.grayDisplayMs += snapshot.phases.grayDisplayMs;
    aggregate.totalPhases.bwRestoreMs += snapshot.phases.bwRestoreMs;
    aggregate.totalPhases.totalMs += snapshot.phases.totalMs;

    aggregate.totalFontCacheHits += snapshot.fontCacheHits;
    aggregate.totalFontCacheMisses += snapshot.fontCacheMisses;
    aggregate.totalFontDecompressMs += snapshot.fontDecompressMs;
    aggregate.totalFontGetBitmapTimeUs += snapshot.fontGetBitmapTimeUs;
    aggregate.totalFontGetBitmapCalls += snapshot.fontGetBitmapCalls;

    if (aggregate.renderCount == 1 || snapshot.freeHeapAfter < aggregate.minFreeHeapAfter) {
      aggregate.minFreeHeapAfter = snapshot.freeHeapAfter;
    }
    if (snapshot.freeHeapAfter > aggregate.maxFreeHeapAfter) {
      aggregate.maxFreeHeapAfter = snapshot.freeHeapAfter;
    }
  };
  const unsigned long startTime = millis();
  int forwardTurns = 0;
  int backwardTurns = 0;

  for (int i = 0; i < 10; i++) {
    if (!stepPageState(true)) {
      break;
    }
    requestUpdateAndWait();
    recordRender(lastRenderStats);
    forwardTurns++;
  }

  const unsigned long forwardMs = millis() - startTime;
  const unsigned long backwardStart = millis();

  for (int i = 0; i < 10; i++) {
    if (!stepPageState(false)) {
      break;
    }
    requestUpdateAndWait();
    recordRender(lastRenderStats);
    backwardTurns++;
  }

  const unsigned long backwardMs = millis() - backwardStart;

  startActivityForResult(
      std::make_unique<EpubRenderBenchmarkActivity>(
          renderer, mappedInput,
          buildRenderBenchmarkReport(startSnapshot, aggregate, forwardTurns, forwardMs, backwardTurns, backwardMs)),
      [this](const ActivityResult&) { requestUpdate(); });
}

std::string EpubReaderActivity::buildRenderBenchmarkReport(const LastRenderStats& startSnapshot,
                                                           const BenchmarkAggregate& aggregate, const int forwardTurns,
                                                           const unsigned long forwardMs, const int backwardTurns,
                                                           const unsigned long backwardMs) const {
  const LastRenderStats& endSnapshot = lastRenderStats.valid ? lastRenderStats : startSnapshot;

  std::string report;
  report.reserve(768);

  auto appendLine = [&report](const std::string& line) {
    if (!report.empty()) {
      report += '\n';
    }
    report += line;
  };

  appendLine("Forward 10: " + std::to_string(forwardTurns) + " turns in " + std::to_string(forwardMs) + " ms");
  if (forwardTurns > 0) {
    appendLine("Forward avg: " + std::to_string(forwardMs / static_cast<unsigned long>(forwardTurns)) + " ms/turn");
  }
  appendLine("Backward 10: " + std::to_string(backwardTurns) + " turns in " + std::to_string(backwardMs) + " ms");
  if (backwardTurns > 0) {
    appendLine("Backward avg: " + std::to_string(backwardMs / static_cast<unsigned long>(backwardTurns)) + " ms/turn");
  }
  appendLine("Measured renders: " + std::to_string(aggregate.renderCount) + ", image pages " +
             std::to_string(aggregate.imagePageCount) + ", cache rebuilds " +
             std::to_string(aggregate.cacheRebuildCount));

  appendLine("Start: spine " + std::to_string(startSnapshot.spineIndex) + ", page " +
             std::to_string(startSnapshot.pageIndex + 1) + "/" + std::to_string(startSnapshot.pageCount));
  appendLine("End: spine " + std::to_string(endSnapshot.spineIndex) + ", page " +
             std::to_string(endSnapshot.pageIndex + 1) + "/" + std::to_string(endSnapshot.pageCount));
  appendLine("Orientation: " +
             std::string(orientationToString(static_cast<GfxRenderer::Orientation>(endSnapshot.orientation))));
  appendLine("Viewport: " + std::to_string(endSnapshot.viewportWidth) + "x" +
             std::to_string(endSnapshot.viewportHeight) + " px, margins T/R/B/L " +
             std::to_string(endSnapshot.marginTop) + "/" + std::to_string(endSnapshot.marginRight) + "/" +
             std::to_string(endSnapshot.marginBottom) + "/" + std::to_string(endSnapshot.marginLeft));
  appendLine("Font: " + std::to_string(endSnapshot.effectiveFontId) + ", embedded CSS " +
             std::string(endSnapshot.embeddedStyle ? "on" : "off") + ", images " +
             std::to_string(endSnapshot.imageRendering) + ", AA " +
             std::string(endSnapshot.textAntiAliasing ? "on" : "off"));
  appendLine("Last page: images " + std::string(endSnapshot.hadImages ? "yes" : "no") + ", footnotes " +
             std::to_string(endSnapshot.footnoteCount) + ", cache rebuilt " +
             std::string(endSnapshot.cacheRebuilt ? "yes" : "no"));
  if (aggregate.renderCount > 0) {
    appendLine("Render avg/min/max: request " +
               std::to_string(aggregate.totalRequestRenderMs / static_cast<unsigned long>(aggregate.renderCount)) +
               "/" + std::to_string(aggregate.minRequestRenderMs) + "/" + std::to_string(aggregate.maxRequestRenderMs) +
               " ms, core " +
               std::to_string(aggregate.totalRenderMs / static_cast<unsigned long>(aggregate.renderCount)) + "/" +
               std::to_string(aggregate.minRenderMs) + "/" + std::to_string(aggregate.maxRenderMs) + " ms");
    appendLine("Aggregate loads: section " + std::to_string(aggregate.totalSectionLoadMs) + " ms, page " +
               std::to_string(aggregate.totalPageLoadMs) + " ms, max footnotes " +
               std::to_string(aggregate.maxFootnotes));
    appendLine("Aggregate phases: prewarm " + std::to_string(aggregate.totalPhases.prewarmMs) + ", bw " +
               std::to_string(aggregate.totalPhases.bwRenderMs) + ", display " +
               std::to_string(aggregate.totalPhases.displayMs) + ", gray total " +
               std::to_string(aggregate.totalPhases.grayLsbMs + aggregate.totalPhases.grayMsbMs +
                              aggregate.totalPhases.grayDisplayMs));
    appendLine("Aggregate font: hits " + std::to_string(aggregate.totalFontCacheHits) + ", misses " +
               std::to_string(aggregate.totalFontCacheMisses) + ", decompress " +
               std::to_string(aggregate.totalFontDecompressMs) + " ms");
    appendLine("Aggregate glyph lookups: " + std::to_string(aggregate.totalFontGetBitmapCalls) + " calls, " +
               std::to_string(aggregate.totalFontGetBitmapTimeUs) + " us total");
    appendLine("Heap after render min/max: " + std::to_string(aggregate.minFreeHeapAfter) + "/" +
               std::to_string(aggregate.maxFreeHeapAfter));
  }
  appendLine("Last render: request " + std::to_string(endSnapshot.requestRenderMs) + " ms, section load " +
             std::to_string(endSnapshot.sectionLoadMs) + " ms, page load " + std::to_string(endSnapshot.pageLoadMs) +
             " ms, render total " + std::to_string(endSnapshot.phases.totalMs) + " ms");
  appendLine("Phases: prewarm " + std::to_string(endSnapshot.phases.prewarmMs) + ", bw " +
             std::to_string(endSnapshot.phases.bwRenderMs) + ", display " +
             std::to_string(endSnapshot.phases.displayMs) + ", store " + std::to_string(endSnapshot.phases.bwStoreMs) +
             ", gray lsb " + std::to_string(endSnapshot.phases.grayLsbMs) + ", gray msb " +
             std::to_string(endSnapshot.phases.grayMsbMs) + ", gray display " +
             std::to_string(endSnapshot.phases.grayDisplayMs) + ", restore " +
             std::to_string(endSnapshot.phases.bwRestoreMs));
  appendLine("Font cache: hits " + std::to_string(endSnapshot.fontCacheHits) + ", misses " +
             std::to_string(endSnapshot.fontCacheMisses) + ", decompress " +
             std::to_string(endSnapshot.fontDecompressMs) + " ms, groups " +
             std::to_string(endSnapshot.fontUniqueGroups));
  appendLine("Font buffers: page " + std::to_string(endSnapshot.fontPageBufferBytes) + ", glyph table " +
             std::to_string(endSnapshot.fontPageGlyphsBytes) + ", peak temp " +
             std::to_string(endSnapshot.fontPeakTempBytes));
  appendLine("Glyph lookups: " + std::to_string(endSnapshot.fontGetBitmapCalls) + " calls, " +
             std::to_string(endSnapshot.fontGetBitmapTimeUs) + " us total");
  appendLine("Heap: before " + std::to_string(endSnapshot.freeHeapBefore) + "/" +
             std::to_string(endSnapshot.largestFreeBlockBefore) + ", after " +
             std::to_string(endSnapshot.freeHeapAfter) + "/" + std::to_string(endSnapshot.largestFreeBlockAfter));

  return report;
}

void EpubReaderActivity::launchKOReaderSync(const SyncLaunchMode mode) {
  if (!epub) {
    return;
  }

  const int currentPage = section ? section->currentPage : 0;
  const int totalPages = section ? section->pageCount : 0;
  KOReaderSyncIntentState syncIntent = KOReaderSyncIntentState::COMPARE;
  if (mode == SyncLaunchMode::PULL_REMOTE) {
    syncIntent = KOReaderSyncIntentState::PULL_REMOTE;
  } else if (mode == SyncLaunchMode::PUSH_LOCAL) {
    syncIntent = KOReaderSyncIntentState::PUSH_LOCAL;
  } else if (mode == SyncLaunchMode::AUTO_PUSH) {
    syncIntent = KOReaderSyncIntentState::AUTO_PUSH;
  }

  auto& sync = APP_STATE.koReaderSyncSession;
  sync.active = true;
  sync.epubPath = epub->getPath();
  sync.spineIndex = currentSpineIndex;
  sync.page = currentPage;
  sync.totalPagesInSpine = totalPages;
  // Populate paragraph index and XHTML seek hint from section LUT if available.
  if (section) {
    if (const auto pIdx = section->getParagraphIndexForPage(static_cast<uint16_t>(currentPage))) {
      sync.paragraphIndex = *pIdx;
      sync.hasParagraphIndex = true;
      if (const auto hint = section->getXhtmlByteOffsetForPage(static_cast<uint16_t>(currentPage))) {
        sync.xhtmlSeekHint = *hint;
      } else {
        sync.xhtmlSeekHint = 0;
      }
    } else {
      sync.paragraphIndex = 0;
      sync.hasParagraphIndex = false;
      sync.xhtmlSeekHint = 0;
    }
  } else {
    sync.paragraphIndex = 0;
    sync.hasParagraphIndex = false;
    sync.xhtmlSeekHint = 0;
  }
  sync.intent = syncIntent;
  sync.outcome = KOReaderSyncOutcomeState::PENDING;
  sync.resultSpineIndex = 0;
  sync.resultPage = 0;
  sync.resultParagraphIndex = 0;
  sync.resultHasParagraphIndex = false;
  // Only auto-push-on-close should bypass the reader on resume; explicit syncs from the
  // reader menu always come back to the reader. Reset here so a stale flag from a prior
  // run cannot steal the user back to home.
  sync.exitToHomeAfterSync = (mode == SyncLaunchMode::AUTO_PUSH);
  APP_STATE.saveToFile();

  LOG_DBG("ERS", "Standalone sync handoff: spine=%d page=%d/%d", currentSpineIndex, currentPage, totalPages);
  logReaderMemSnapshot("before_replace_with_sync");
  activityManager.goToKOReaderSync();
}

bool EpubReaderActivity::tryAutoPushOnClose() {
  // Three-page minimum filters out brief inspections — opening to check the cover or
  // skim the TOC shouldn't burn a network round-trip. Counter is per-activity-instance.
  constexpr int MIN_SESSION_PAGES = 3;
  if (!SETTINGS.koSyncOnBookClose) {
    return false;
  }
  if (!KOREADER_STORE.hasCredentials()) {
    return false;
  }
  if (sessionPagesAdvanced < MIN_SESSION_PAGES) {
    return false;
  }
  if (!epub) {
    return false;
  }

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0 || currentSpineIndex >= spineCount || !section) {
    LOG_DBG("ERS", "Skipping AUTO_PUSH on end-of-book sentinel: spine=%d section=%s", currentSpineIndex,
            section ? "present" : "null");
    return false;
  }

  // exitToHomeAfterSync flag is set inside launchKOReaderSync for AUTO_PUSH mode.
  launchKOReaderSync(SyncLaunchMode::AUTO_PUSH);
  return true;
}

void EpubReaderActivity::applyPendingSyncSession() {
  auto& sync = APP_STATE.koReaderSyncSession;
  if (!sync.active || !epub || sync.epubPath != epub->getPath()) {
    return;
  }

  LOG_DBG("ERS", "Applying pending sync session outcome=%d path=%s", static_cast<int>(sync.outcome),
          sync.epubPath.c_str());

  // Upload-complete returns to the same local position the reader already persisted
  // before sync launched, so there is no need to rewrite progress.bin here.
  if (sync.outcome == KOReaderSyncOutcomeState::UPLOAD_COMPLETE) {
    LOG_DBG("ERS", "Upload-complete resume keeps existing local progress.bin unchanged");
    sync.clear();
    APP_STATE.saveToFile();
    logReaderMemSnapshot("after_apply_pending_sync_session");
    return;
  }

  // AUTO_PULL handed off zeroed local state (the reader was not yet running when sync started),
  // so on cancel/fail we must NOT restore those zeros to progress.bin — they would clobber the
  // user's real local progress. Just clear the session and let the normal startup load progress.bin.
  if (sync.intent == KOReaderSyncIntentState::AUTO_PULL && sync.outcome != KOReaderSyncOutcomeState::APPLIED_REMOTE) {
    LOG_DBG("ERS", "AUTO_PULL non-success outcome=%d: leaving progress.bin untouched", static_cast<int>(sync.outcome));
    sync.clear();
    APP_STATE.saveToFile();
    logReaderMemSnapshot("after_apply_pending_sync_session");
    return;
  }

  int restoreSpineIndex = sync.spineIndex;
  int restorePage = sync.page;

  if (restoreSpineIndex < 0 || restoreSpineIndex >= epub->getSpineItemsCount()) {
    LOG_ERR("ERS", "Invalid sync restore spine index %d, resetting to 0", restoreSpineIndex);
    restoreSpineIndex = 0;
    restorePage = 0;
  }

  // Build the navigation target from the sync result. For LUT-anchored targets the
  // estimated restorePage is plumbed through as fallbackPage so a LUT miss in the
  // target spine still lands the user on a sensible page rather than page 0.
  NavigationTarget restoreTarget;
  if (sync.outcome == KOReaderSyncOutcomeState::APPLIED_REMOTE) {
    const int spineCount = epub->getSpineItemsCount();
    if (sync.resultSpineIndex < 0 || sync.resultSpineIndex >= spineCount) {
      LOG_ERR("ERS", "Sync resultSpineIndex %d out of range [0,%d), clamping to previous %d", sync.resultSpineIndex,
              spineCount, restoreSpineIndex);
      // Keep restoreSpineIndex / restorePage from the pre-validation block above.
    } else {
      restoreSpineIndex = sync.resultSpineIndex;
      restorePage = sync.resultPage;
    }
    if (sync.resultHasListItemIndex) {
      restoreTarget = NavigationTarget::makeListItem(sync.resultListItemIndex, restorePage);
      LOG_DBG("ERS", "Applied synced remote position: spine=%d page=%d li[%u]", restoreSpineIndex, restorePage,
              sync.resultListItemIndex);
    } else if (sync.resultHasParagraphIndex) {
      restoreTarget = NavigationTarget::makeParagraph(sync.resultParagraphIndex, restorePage);
      LOG_DBG("ERS", "Applied synced remote position: spine=%d page=%d p[%u]", restoreSpineIndex, restorePage,
              sync.resultParagraphIndex);
    } else {
      restoreTarget = NavigationTarget::makePage(restorePage);
      LOG_DBG("ERS", "Applied synced remote position: spine=%d page=%d (no LUT)", restoreSpineIndex, restorePage);
    }
  } else {
    restoreTarget = NavigationTarget::makePage(restorePage);
    LOG_DBG("ERS", "Restored local pre-sync position: spine=%d page=%d", restoreSpineIndex, restorePage);
  }

  // sync.totalPagesInSpine is the page count of the local spine at launch time.
  // When the restore targets a different spine, that count is meaningless for
  // rescaling the fallbackPage estimate (which was estimated from cross-spine
  // density anyway). Store 0 to disable rescaling — the LUT lookup is the precise
  // path, and the cross-spine fallback can't usefully be rescaled here.
  const int restorePageCount = (restoreSpineIndex == sync.spineIndex) ? sync.totalPagesInSpine : 0;
  restoreTarget.cachedPageCount = restorePageCount;
  restoreTarget.cachedSpineIdx = restoreSpineIndex;

  // Seed live state directly — the previous write-then-reload-from-disk pattern relied
  // on progress.bin being read after this function ran, which clobbered the LUT target.
  // Live-state seeding is authoritative; the persistent write below is just for crash
  // recovery so a power loss before the next saveProgress() doesn't lose the synced
  // spine/page. The next render's saveProgress() supplies the real percent before
  // the user can return to the home screen.
  currentSpineIndex = restoreSpineIndex;
  navTarget = restoreTarget;
  if (!writeReaderProgressCache(epub->getCachePath(), restoreSpineIndex, restorePage, restorePageCount, 0)) {
    LOG_ERR("ERS", "Failed to persist sync restore to progress.bin; live state still seeded");
  } else {
    LOG_DBG("ERS", "Prepared progress.bin for sync restore: spine=%d page=%d/%d", restoreSpineIndex, restorePage,
            sync.totalPagesInSpine);
  }

  sync.clear();
  APP_STATE.saveToFile();
  logReaderMemSnapshot("after_apply_pending_sync_session");
}

void EpubReaderActivity::applyPendingBookmarkJump() {
  auto& jump = APP_STATE.pendingBookmarkJump;
  if (!jump.active || !epub || jump.bookPath != epub->getPath()) {
    return;
  }
  LOG_DBG("ERS", "Applying pending bookmark jump: spine=%u page=%u", jump.spineIndex, jump.pageNumber);
  if (jump.spineIndex >= static_cast<uint16_t>(epub->getSpineItemsCount())) {
    LOG_ERR("ERS", "Invalid bookmark jump spine index %u, resetting to 0", jump.spineIndex);
    jump.spineIndex = 0;
    jump.pageNumber = 0;
  }
  // Seed live state directly; the persistent write is for crash recovery only.
  // saveProgress() on the next render overwrites with the real percent.
  currentSpineIndex = jump.spineIndex;
  navTarget = NavigationTarget::makePage(jump.pageNumber);
  navTarget.cachedSpineIdx = jump.spineIndex;
  if (!writeReaderProgressCache(epub->getCachePath(), jump.spineIndex, jump.pageNumber, 0, 0)) {
    LOG_ERR("ERS", "Failed to persist bookmark jump to progress.bin; live state still seeded");
  }
  jump.clear();
  APP_STATE.saveToFile();
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  // No-op if the selected orientation matches current settings.
  if (SETTINGS.orientation == orientation) {
    return;
  }

  // Preserve current reading position so we can restore after reflow.
  {
    RenderLock lock(*this);
    if (section) {
      navTarget = NavigationTarget::makePage(section->currentPage);
      navTarget.cachedPageCount = section->pageCount;
      navTarget.cachedSpineIdx = currentSpineIndex;
    }

    // Persist the selection so the reader keeps the new orientation on next launch.
    SETTINGS.orientation = orientation;
    SETTINGS.saveToFile();

    // Update renderer orientation to match the new logical coordinate system.
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

    // Reset section to force re-layout in the new orientation.
    section.reset();
  }
}

void EpubReaderActivity::applyTextDarkness(const uint8_t textDarkness) {
  if (SETTINGS.textDarkness == textDarkness) {
    return;
  }
  SETTINGS.textDarkness = textDarkness;
  SETTINGS.saveToFile();
  renderer.setTextDarkness(textDarkness);
  // Force a re-render so the new darkness is visible immediately.
  requestUpdate();
}

void EpubReaderActivity::stopAutomaticPageTurn() {
  if (!automaticPageTurnActive) {
    return;
  }

  automaticPageTurnActive = false;

  if (UITheme::getStatusBarHeight(true) == UITheme::getStatusBarHeight()) {
    return;
  }

  // Preserve current reading position so we can restore after reflow.
  RenderLock lock(*this);
  if (section) {
    navTarget = NavigationTarget::makePage(section->currentPage);
    navTarget.cachedPageCount = section->pageCount;
    navTarget.cachedSpineIdx = currentSpineIndex;
  }
  section.reset();
}

void EpubReaderActivity::toggleAutoPageTurn(const uint8_t selectedPageTurnOption) {
  if (selectedPageTurnOption == 0 || selectedPageTurnOption >= std::size(PAGE_TURN_LABELS)) {
    stopAutomaticPageTurn();
    return;
  }

  lastPageTurnTime = millis();
  // calculates page turn duration by dividing by number of pages
  pageTurnDuration = (1UL * 60 * 1000) / PAGE_TURN_LABELS[selectedPageTurnOption];
  automaticPageTurnActive = true;

  // Reset cached section when automatic page turn adds a forced status item band.
  if (UITheme::getStatusBarHeight(true) != UITheme::getStatusBarHeight()) {
    // Preserve current reading position so we can restore after reflow.
    RenderLock lock(*this);
    if (section) {
      navTarget = NavigationTarget::makePage(section->currentPage);
      navTarget.cachedPageCount = section->pageCount;
      navTarget.cachedSpineIdx = currentSpineIndex;
    }
    section.reset();
  }
}

void EpubReaderActivity::applyBookReaderOverrides(const int8_t embeddedStyleOverride,
                                                  const int8_t imageRenderingOverride, const int8_t fontFamilyOverride,
                                                  const std::string& sdFontFamilyOverride,
                                                  const int8_t fontSizeOverride, const bool bionicReadingOverride,
                                                  const int8_t paragraphAlignmentOverride) {
  if (!epub) {
    return;
  }

  // Built-in and SD font overrides are mutually exclusive; explicit built-in wins.
  int8_t normalizedFontFamilyOverride = fontFamilyOverride;
  std::string normalizedSdFontFamilyOverride = sdFontFamilyOverride;
  if (normalizedFontFamilyOverride >= 0) {
    normalizedSdFontFamilyOverride.clear();
  } else if (!normalizedSdFontFamilyOverride.empty()) {
    normalizedFontFamilyOverride = -1;
  }

  if (bookEmbeddedStyleOverride == embeddedStyleOverride && bookImageRenderingOverride == imageRenderingOverride &&
      bookFontFamilyOverride == normalizedFontFamilyOverride &&
      bookSdFontFamilyOverride == normalizedSdFontFamilyOverride && bookFontSizeOverride == fontSizeOverride &&
      bookBionicReadingOverride == bionicReadingOverride &&
      bookParagraphAlignmentOverride == paragraphAlignmentOverride) {
    return;
  }

  bookEmbeddedStyleOverride = embeddedStyleOverride;
  bookImageRenderingOverride = imageRenderingOverride;
  bookFontFamilyOverride = normalizedFontFamilyOverride;
  bookSdFontFamilyOverride = normalizedSdFontFamilyOverride;
  bookFontSizeOverride = fontSizeOverride;
  bookBionicReadingOverride = bionicReadingOverride;
  bookParagraphAlignmentOverride = paragraphAlignmentOverride;
  RECENT_BOOKS.setReaderOverrides(epub->getPath(), bookEmbeddedStyleOverride, bookImageRenderingOverride,
                                  bookFontFamilyOverride, bookSdFontFamilyOverride, bookFontSizeOverride,
                                  bookBionicReadingOverride, bookParagraphAlignmentOverride);

  RenderLock lock(*this);
  if (section) {
    navTarget = NavigationTarget::makePage(section->currentPage);
    navTarget.cachedPageCount = section->pageCount;
    navTarget.cachedSpineIdx = currentSpineIndex;
  }
  section.reset();
}

bool EpubReaderActivity::getEffectiveEmbeddedStyle() const {
  if (bookEmbeddedStyleOverride >= 0) {
    return bookEmbeddedStyleOverride != 0;
  }
  return SETTINGS.embeddedStyle != 0;
}

uint8_t EpubReaderActivity::getEffectiveImageRendering() const {
  if (bookImageRenderingOverride >= 0) {
    return static_cast<uint8_t>(bookImageRenderingOverride);
  }
  return SETTINGS.imageRendering;
}

uint8_t EpubReaderActivity::getEffectiveParagraphAlignment() const {
  if (bookParagraphAlignmentOverride >= 0) {
    return static_cast<uint8_t>(bookParagraphAlignmentOverride);
  }
  return SETTINGS.paragraphAlignment;
}

float EpubReaderActivity::getEffectiveReaderLineCompression() const {
  const uint8_t fontSize = (bookFontSizeOverride >= 0) ? static_cast<uint8_t>(bookFontSizeOverride) : SETTINGS.fontSize;
  const int effectiveFontId = getEffectiveReaderFontId();
  const int notosansId = CrossPointSettings::getBuiltinReaderFontId(CrossPointSettings::NOTOSANS, fontSize);

  if (effectiveFontId == notosansId) {
    switch (SETTINGS.lineSpacing) {
      case CrossPointSettings::TIGHT:
        return 0.90f;
      case CrossPointSettings::NORMAL:
      default:
        return 0.95f;
      case CrossPointSettings::WIDE:
        return 1.0f;
    }
  }

  switch (SETTINGS.lineSpacing) {
    case CrossPointSettings::TIGHT:
      return 0.95f;
    case CrossPointSettings::NORMAL:
    default:
      return 1.0f;
    case CrossPointSettings::WIDE:
      return 1.1f;
  }
}

int EpubReaderActivity::getEffectiveReaderFontId() const {
  // Per-book font override: when set, force a specific BUILT-IN family even if
  // an SD card font is the global default. This makes the override predictable
  // ("override forces back to a known built-in") and avoids surprising users
  // who set the override before they had any SD fonts.
  const uint8_t fontSize = (bookFontSizeOverride >= 0) ? static_cast<uint8_t>(bookFontSizeOverride) : SETTINGS.fontSize;
  if (bookFontFamilyOverride >= 0) {
    return CrossPointSettings::getBuiltinReaderFontId(static_cast<uint8_t>(bookFontFamilyOverride), fontSize);
  }
  if (!bookSdFontFamilyOverride.empty()) {
    const int id = resolveSdCardFontId(bookSdFontFamilyOverride.c_str(), fontSize);
    if (id != 0) return id;
  }
  // No override: defer to global resolution (which honors SD card font selection).
  // We synthesize a temporary lookup using the override fontSize if it's set; otherwise
  // SETTINGS.getReaderFontId() is the canonical answer.
  if (bookFontSizeOverride >= 0) {
    if (SETTINGS.sdFontFamilyName[0] != '\0') {
      const int id = resolveSdCardFontId(SETTINGS.sdFontFamilyName, fontSize);
      if (id != 0) return id;
    }
    return CrossPointSettings::getBuiltinReaderFontId(SETTINGS.fontFamily, fontSize);
  }
  return SETTINGS.getReaderFontId();
}

void EpubReaderActivity::NavigationTarget::resolveInto(Section& sec, int spineIndex) const {
  // Resolve to a baseline page first. Each branch records whether it produced a
  // precise page (LUT/anchor hit, percent jump, explicit page) or only an estimate.
  // The estimate path runs cross-spine rescale + clamp at the end; the precise path
  // skips both because LUT pages are already in the target spine's coordinate system.
  bool isEstimate = false;

  switch (kind) {
    case Kind::LastPage: {
      sec.currentPage = (sec.pageCount > 0) ? sec.pageCount - 1 : 0;
      break;
    }

    case Kind::TocIndex: {
      if (const auto p = sec.getPageForTocIndex(tocIndex)) {
        sec.currentPage = *p;
      }
      break;
    }

    case Kind::Anchor: {
      if (const auto p = sec.getPageForAnchor(anchorStr)) {
        sec.currentPage = *p;
        LOG_DBG("ERS", "Resolved anchor '%s' -> page %d", anchorStr.c_str(), *p);
      } else {
        LOG_DBG("ERS", "Anchor '%s' not found; using fallback page %d", anchorStr.c_str(), fallbackPage);
        sec.currentPage = fallbackPage;
        isEstimate = true;
      }
      break;
    }

    case Kind::ListItem: {
      if (const auto p = sec.getPageForListItemIndex(lutIndex)) {
        sec.currentPage = *p;
        LOG_DBG("ERS", "Resolved li[%u] -> page %d", lutIndex, *p);
      } else if (const auto pp = sec.getPageForParagraphIndex(lutIndex)) {
        // Some <li>-anchored XPaths land in books where the LI LUT is empty (no <li>
        // inside <body>'s direct children, or all <li>s skipped). Fall back to the
        // paragraph LUT — the running indices coincide often enough to help, and
        // it's strictly better than dropping back to the estimate.
        sec.currentPage = *pp;
        LOG_DBG("ERS", "Li LUT miss for li[%u]; paragraph LUT -> page %d", lutIndex, *pp);
      } else {
        LOG_DBG("ERS", "Li[%u] not in LUT; using fallback page %d", lutIndex, fallbackPage);
        sec.currentPage = fallbackPage;
        isEstimate = true;
      }
      break;
    }

    case Kind::Paragraph: {
      if (const auto p = sec.getPageForParagraphIndex(lutIndex)) {
        sec.currentPage = *p;
        LOG_DBG("ERS", "Resolved p[%u] -> page %d", lutIndex, *p);
      } else {
        LOG_DBG("ERS", "Paragraph LUT miss for p[%u]; using fallback page %d", lutIndex, fallbackPage);
        sec.currentPage = fallbackPage;
        isEstimate = true;
      }
      break;
    }

    case Kind::Percent: {
      if (sec.pageCount > 0) {
        int newPage = static_cast<int>(spineProgress * static_cast<float>(sec.pageCount));
        if (newPage >= sec.pageCount) newPage = sec.pageCount - 1;
        sec.currentPage = newPage;
      }
      break;
    }

    case Kind::Page: {
      sec.currentPage = page;
      isEstimate = true;
      break;
    }
  }

  // Cross-font / cross-spine rescaling: only for estimated pages. cachedPageCount
  // is the page count at the time the estimate was made — when it disagrees with
  // the section's current page count (reflow / different spine entirely), rescale
  // the estimate proportionally before clamping.
  if (isEstimate && cachedPageCount > 0 && cachedSpineIdx == spineIndex && sec.pageCount != cachedPageCount) {
    const float progress = static_cast<float>(sec.currentPage) / static_cast<float>(cachedPageCount);
    sec.currentPage = static_cast<int>(progress * static_cast<float>(sec.pageCount));
  }

  // Safety clamp for all paths — a LUT-derived page is also defensively clamped in
  // case the cache is somehow stale.
  if (sec.currentPage < 0) {
    LOG_DBG("ERS", "Clamping negative page %d to 0 (spine=%d cachedPageCount=%d)", sec.currentPage, spineIndex,
            cachedPageCount);
    sec.currentPage = 0;
  }
  if (sec.pageCount > 0 && sec.currentPage >= sec.pageCount) {
    LOG_DBG("ERS", "Clamping page %d to last page %d", sec.currentPage, sec.pageCount - 1);
    sec.currentPage = sec.pageCount - 1;
  }
}

bool EpubReaderActivity::stepPageState(const bool isForwardTurn) {
  if (!epub || !section || section->pageCount == 0) {
    return false;
  }

  if (isForwardTurn) {
    if (section->currentPage < section->pageCount - 1) {
      section->currentPage++;
    } else if (currentSpineIndex + 1 < epub->getSpineItemsCount()) {
      RenderLock lock(*this);
      navTarget = NavigationTarget::makePage(0);
      currentSpineIndex++;
      section.reset();
    } else if (currentSpineIndex + 1 == epub->getSpineItemsCount()) {
      RenderLock lock(*this);
      navTarget = NavigationTarget::makeLastPage();
      currentSpineIndex++;
      section.reset();
    } else {
      return false;
    }
  } else {
    if (section->currentPage > 0) {
      section->currentPage--;
    } else if (currentSpineIndex > 0) {
      RenderLock lock(*this);
      navTarget = NavigationTarget::makeLastPage();
      currentSpineIndex--;
      section.reset();
    } else {
      return false;
    }
  }

  lastPageTurnTime = millis();
  forceLoadLargeImages = false;
  pageHasPlaceholders = false;
  return true;
}

void EpubReaderActivity::pageTurn(bool isForwardTurn) {
  if (isForwardTurn && section && preRenderedPage.ready && preRenderedPage.spineIndex == currentSpineIndex &&
      preRenderedPage.pageIndex == section->currentPage + 1) {
    // Fast path: the frame buffer already holds the next page content. Advance state here on the
    // loop task, then hand off to render() via usePreRenderedBuffer — all display work (status
    // bar, flush, AA pass) stays on the render task where it belongs. No RenderLock acquired here.
    section->currentPage = preRenderedPage.pageIndex;
    preRenderedPage.ready = false;
    usePreRenderedBuffer = true;
    sessionPagesAdvanced++;
    globalReadingSessionTracker().onPageTurn();
    lastPageTurnTime = millis();
    requestUpdate();
    return;
  }

  if (!stepPageState(isForwardTurn)) {
    return;
  }
  sessionPagesAdvanced++;
  globalReadingSessionTracker().onPageTurn();
  preRenderedPage.ready = false;
  requestUpdate();
}

// TODO: Failure handling
void EpubReaderActivity::render(RenderLock&& lock) {
  if (!epub) {
    return;
  }
  logIntegrityProbe("render_entry");

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount <= 0) {
    LOG_ERR("ERS", "EPUB has no spine items, aborting render");
    automaticPageTurnActive = false;
    return;
  }

  // edge case handling for sub-zero spine index
  if (currentSpineIndex < 0) {
    currentSpineIndex = 0;
  }
  // based bounds of book, show end of book screen
  if (currentSpineIndex > spineCount) {
    currentSpineIndex = spineCount;
  }

  // Immediately transition to finished-book flow instead of showing an end-of-book screen
  if (currentSpineIndex == spineCount) {
    if (!finishedBookActivityStarted_) {
      finishedBookActivityStarted_ = true;
      const int lastSpineIndex = std::max(0, spineCount - 1);
      writeReaderProgressCache(epub->getCachePath(), lastSpineIndex, 0, 0, 100);
      const std::string nextBookPath =
          BookFinished::findNextBookInDirectory(epub->getPath(), epub->getSeries(), epub->getSeriesIndex());
      lock.unlock();
      startActivityForResult(
          std::make_unique<FinishedBookActivity>(renderer, mappedInput, epub->getPath(), nextBookPath),
          [this, nextBookPath](const ActivityResult& result) {
            finishedBookActivityStarted_ = false;
            if (result.isCancelled) {
              requestUpdate();
              return;
            }
            globalReadingSessionTracker().markFinished();
            const auto& menuResult = std::get<MenuResult>(result.data);
            if (menuResult.action == static_cast<int>(BookFinished::FinishedBookAction::GoHome)) {
              if (SETTINGS.moveFinishedBooksToCompleted) {
                std::string movedPath;
                BookFinished::moveFinishedBookToCompleted(epub->getPath(), movedPath);
              }
              if (SETTINGS.removeFinishedBooksFromRecents) {
                RECENT_BOOKS.removeBook(epub->getPath());
              }
              activityManager.goHome();
              return;
            }
            if (menuResult.action == static_cast<int>(BookFinished::FinishedBookAction::OpenNextBook) &&
                !nextBookPath.empty()) {
              if (SETTINGS.moveFinishedBooksToCompleted) {
                std::string movedPath;
                BookFinished::moveFinishedBookToCompleted(epub->getPath(), movedPath);
              }
              if (SETTINGS.removeFinishedBooksFromRecents) {
                RECENT_BOOKS.removeBook(epub->getPath());
              }
              activityManager.goToReader(nextBookPath);
              return;
            }
            requestUpdate();
          });
    }
    return;
  }

  // Apply screen viewable areas and additional padding
  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  const int statusBarTopHeight = UITheme::getStatusBarTopHeight(automaticPageTurnActive);
  const int statusBarBottomHeight = UITheme::getStatusBarBottomHeight(automaticPageTurnActive);

  orientedMarginTop += std::max(static_cast<int>(SETTINGS.screenMargin), statusBarTopHeight);
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;
  orientedMarginBottom += std::max(static_cast<int>(SETTINGS.screenMargin), statusBarBottomHeight);

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;
  lastRenderStats = {};
  lastRenderStats.orientation = static_cast<uint8_t>(renderer.getOrientation());
  lastRenderStats.marginTop = orientedMarginTop;
  lastRenderStats.marginRight = orientedMarginRight;
  lastRenderStats.marginBottom = orientedMarginBottom;
  lastRenderStats.marginLeft = orientedMarginLeft;
  lastRenderStats.viewportWidth = viewportWidth;
  lastRenderStats.viewportHeight = viewportHeight;
  lastRenderStats.embeddedStyle = getEffectiveEmbeddedStyle();
  lastRenderStats.imageRendering = getEffectiveImageRendering();
  lastRenderStats.effectiveFontId = getEffectiveReaderFontId();
  lastRenderStats.textAntiAliasing = SETTINGS.textAntiAliasing;
  lastRenderStats.freeHeapBefore = esp_get_free_heap_size();
  lastRenderStats.largestFreeBlockBefore = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
  showTruncatedSectionHintThisRender = false;

  // Capture and clear all pre-render flags before any state checks.
  // - isPreRenderPass:     render next page content only (no status bar, no flush).
  // - isBufferDisplayPass: frame buffer already has content; just add status bar and flush.
  // - anything else:       normal full render; discard any stale pre-render state.
  const bool isPreRenderPass = pendingPreRender;
  const bool isBufferDisplayPass = usePreRenderedBuffer;
  pendingPreRender = false;
  usePreRenderedBuffer = false;
  if (!isPreRenderPass && !isBufferDisplayPass) {
    preRenderedPage.ready = false;
  }

  // Fast-display pass: frame buffer holds pre-rendered content; superimpose live status bar,
  // flush to display, and run the AA pass — all on the render task with no SD font re-read.
  if (isBufferDisplayPass) {
    if (section) {
      auto p = section->loadPageFromSectionFile();
      if (p && !p->hasImages()) {
        currentPageFootnotes = std::move(p->footnotes);
        displayPreRenderedPage(*p, orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);

        pendingProgressSave.spineIndex = currentSpineIndex;
        pendingProgressSave.page = section->currentPage;
        pendingProgressSave.pageCount = section->pageCount;
        pendingProgressSave.pending.store(true, std::memory_order_release);

        if (section->currentPage + 1 < section->pageCount) {
          pendingPreRender = true;
          requestUpdate();
        }
        return;
      }
      // Page load failed or was an image page — fall through to full render.
    }
  }

  // Pre-render pass: render next page content into the frame buffer (no status bar, no flush).
  if (isPreRenderPass) {
    if (section && !preRenderedPage.ready) {
      const int nextPage = section->currentPage + 1;
      if (nextPage < section->pageCount) {
        const uint32_t freeHeap = esp_get_free_heap_size();
        const uint32_t contigHeap = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
        if (freeHeap >= SILENT_INDEX_MIN_FREE_HEAP_BYTES && contigHeap >= SILENT_INDEX_MIN_CONTIG_HEAP_BYTES) {
          const int savedPage = section->currentPage;
          section->currentPage = nextPage;
          auto p = section->loadPageFromSectionFile();
          section->currentPage = savedPage;
          if (p && !p->hasImages()) {
            logIntegrityProbe("preRender_before_renderPageContentOnly");
            section->currentPage = nextPage;
            renderPageContentOnly(*p, orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
            section->currentPage = savedPage;
            preRenderedPage = {true, currentSpineIndex, nextPage};
            LOG_DBG("ERS", "Pre-rendered page %d/%d", nextPage, section->pageCount - 1);
            logIntegrityProbe("preRender_after_renderPageContentOnly");
          }
        }
      }
    }
    return;
  }

  if (!section) {
    if (currentSpineIndex < 0 || currentSpineIndex >= spineCount) {
      LOG_ERR("ERS", "Render rejected invalid spine index %d (valid 0..%d)", currentSpineIndex, spineCount - 1);
      currentSpineIndex = 0;
      navTarget = NavigationTarget::makePage(0);
      automaticPageTurnActive = false;
      requestUpdate();
      return;
    }

    const bool embeddedStyle = lastRenderStats.embeddedStyle;
    const uint8_t imageRendering = lastRenderStats.imageRendering;
    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    LOG_DBG("ERS", "Loading file: %s, index: %d", filepath.c_str(), currentSpineIndex);
    section = std::make_unique<Section>(epub, currentSpineIndex, renderer);
    const unsigned long sectionStart = millis();

    if (!section->loadSectionFile(getEffectiveReaderFontId(), getEffectiveReaderLineCompression(),
                                  SETTINGS.extraParagraphSpacing, getEffectiveParagraphAlignment(), viewportWidth,
                                  viewportHeight, SETTINGS.hyphenationEnabled, embeddedStyle, bookBionicReadingOverride,
                                  imageRendering)) {
      LOG_DBG("ERS", "Cache not found, building...");
      lastRenderStats.cacheRebuilt = true;

      Rect popupRect{};
      const auto progressFn = [this, &popupRect](int progress) {
        if (popupRect.width == 0) {
          // Drawing the popup already does a full refresh, which serves as the
          // 0% indication; no need to follow it with a redundant fillPopupProgress.
          popupRect = GUI.drawPopup(renderer, tr(STR_INDEXING));
          return;
        }
        GUI.fillPopupProgress(renderer, popupRect, progress);
      };

      // Reset cumulative SD font metadata cache so this section starts fresh.
      // Pagination will rebuild only the cps it actually encounters, bounded
      // by MAX_PAGE_GLYPHS per style.
      renderer.clearSdCardFontAccumulation();
      if (!section->createSectionFile(getEffectiveReaderFontId(), getEffectiveReaderLineCompression(),
                                      SETTINGS.extraParagraphSpacing, getEffectiveParagraphAlignment(), viewportWidth,
                                      viewportHeight, SETTINGS.hyphenationEnabled, embeddedStyle,
                                      bookBionicReadingOverride, imageRendering, progressFn)) {
        LOG_ERR("ERS", "Failed to persist page data to SD");
        section.reset();
        return;
      }
    } else if (section->isEmbeddedStyleFallback()) {
      LOG_INF("ERS", "No-CSS fallback loaded; rebuilding with embedded CSS...");
      lastRenderStats.cacheRebuilt = true;

      Rect popupRect{};
      const auto progressFn = [this, &popupRect](int progress) {
        if (popupRect.width == 0) {
          popupRect = GUI.drawPopup(renderer, tr(STR_INDEXING));
          return;
        }
        GUI.fillPopupProgress(renderer, popupRect, progress);
      };

      renderer.clearSdCardFontAccumulation();
      if (!section->createSectionFile(getEffectiveReaderFontId(), getEffectiveReaderLineCompression(),
                                      SETTINGS.extraParagraphSpacing, getEffectiveParagraphAlignment(), viewportWidth,
                                      viewportHeight, SETTINGS.hyphenationEnabled, embeddedStyle,
                                      bookBionicReadingOverride, imageRendering, progressFn)) {
        LOG_ERR("ERS", "Failed to rebuild CSS section cache; keeping fallback");
        section->loadSectionFile(getEffectiveReaderFontId(), getEffectiveReaderLineCompression(),
                                 SETTINGS.extraParagraphSpacing, getEffectiveParagraphAlignment(), viewportWidth,
                                 viewportHeight, SETTINGS.hyphenationEnabled, embeddedStyle, bookBionicReadingOverride,
                                 imageRendering);
      }
    } else {
      LOG_DBG("ERS", "Cache found, skipping build...");
    }
    lastRenderStats.sectionLoadMs = millis() - sectionStart;
    logIntegrityProbe("render_after_sectionLoad");

    if (section->isTruncatedCache() && currentSpineIndex != lastWarnedTruncatedSpineIndex) {
      lastWarnedTruncatedSpineIndex = currentSpineIndex;
      truncatedSectionHintRendersRemaining = TRUNCATED_SECTION_HINT_RENDER_COUNT;
      LOG_INF("ERS", "Section %d is truncated; showing mitigation hint", currentSpineIndex);
    }

    navTarget.resolveInto(*section, currentSpineIndex);
    navTarget = NavigationTarget::makePage(section->currentPage);
    forceLoadLargeImages = false;
    pageHasPlaceholders = false;
  }

  renderer.clearScreen();

  if (section->pageCount == 0) {
    LOG_DBG("ERS", "No pages to render");
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    LOG_DBG("ERS", "Page out of bounds: %d (max %d)", section->currentPage, section->pageCount);
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_OUT_OF_BOUNDS), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    return;
  }

  {
    const unsigned long pageLoadStart = millis();
    auto p = section->loadPageFromSectionFile();
    lastRenderStats.pageLoadMs = millis() - pageLoadStart;
    logIntegrityProbe("render_after_pageLoad");
    if (!p) {
      LOG_ERR("ERS", "Failed to load page from SD - clearing section cache");
      section->clearCache();
      section.reset();
      requestUpdate();  // Try again after clearing cache
                        // TODO: prevent infinite loop if the page keeps failing to load for some reason
      automaticPageTurnActive = false;
      return;
    }

    // Collect footnotes from the loaded page
    currentPageFootnotes = std::move(p->footnotes);
    lastRenderStats.hadImages = p->hasImages();
    lastRenderStats.footnoteCount = static_cast<int>(currentPageFootnotes.size());
    lastRenderStats.spineIndex = currentSpineIndex;
    lastRenderStats.pageIndex = section->currentPage;
    lastRenderStats.pageCount = section->pageCount;
    showTruncatedSectionHintThisRender = truncatedSectionHintRendersRemaining > 0;

    const auto start = millis();
    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    lastRenderStats.requestRenderMs = millis() - start;
    if (truncatedSectionHintRendersRemaining > 0) {
      truncatedSectionHintRendersRemaining--;
    }
    LOG_DBG("ERS", "Rendered page in %dms", lastRenderStats.requestRenderMs);
    logIntegrityProbe("render_after_renderContents");
  }
  silentIndexNextChapterIfNeeded(viewportWidth, viewportHeight);
  logIntegrityProbe("render_after_silentIndex");
  pendingProgressSave.spineIndex = currentSpineIndex;
  pendingProgressSave.page = section->currentPage;
  pendingProgressSave.pageCount = section->pageCount;
  pendingProgressSave.pending.store(true, std::memory_order_release);
  lastRenderStats.freeHeapAfter = esp_get_free_heap_size();
  lastRenderStats.largestFreeBlockAfter = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
  lastRenderStats.valid = true;

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }

  // Schedule a pre-render of the next page into the frame buffer so the next forward
  // page turn can skip the render pass and go straight to displayBuffer().
  if (!preRenderedPage.ready && section->currentPage + 1 < section->pageCount) {
    pendingPreRender = true;
    requestUpdate();
  }
}

void EpubReaderActivity::silentIndexNextChapterIfNeeded(const uint16_t viewportWidth, const uint16_t viewportHeight) {
  if (!epub || !section || section->pageCount < 2) {
    return;
  }

  const uint32_t freeHeap = esp_get_free_heap_size();
  const uint32_t contigHeap = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
  if (freeHeap < SILENT_INDEX_MIN_FREE_HEAP_BYTES || contigHeap < SILENT_INDEX_MIN_CONTIG_HEAP_BYTES) {
    LOG_DBG("ERS", "Skipping silent indexing due to low heap (free=%lu contig=%lu)", freeHeap, contigHeap);
    return;
  }

  // Build the next chapter cache while the penultimate page is on screen.
  if (section->currentPage != section->pageCount - 2) {
    return;
  }

  const int nextSpineIndex = currentSpineIndex + 1;
  if (nextSpineIndex < 0 || nextSpineIndex >= epub->getSpineItemsCount()) {
    return;
  }

  const bool embeddedStyle = getEffectiveEmbeddedStyle();
  const uint8_t imageRendering = getEffectiveImageRendering();

  Section nextSection(epub, nextSpineIndex, renderer);
  if (nextSection.loadSectionFile(getEffectiveReaderFontId(), getEffectiveReaderLineCompression(),
                                  SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                  viewportHeight, SETTINGS.hyphenationEnabled, embeddedStyle, bookBionicReadingOverride,
                                  imageRendering)) {
    return;
  }

  LOG_DBG("ERS", "Silently indexing next chapter: %d", nextSpineIndex);
  // Reset cumulative SD font metadata cache for the new section.
  renderer.clearSdCardFontAccumulation();
  if (!nextSection.createSectionFile(getEffectiveReaderFontId(), getEffectiveReaderLineCompression(),
                                     SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth,
                                     viewportHeight, SETTINGS.hyphenationEnabled, embeddedStyle,
                                     bookBionicReadingOverride, imageRendering)) {
    LOG_ERR("ERS", "Failed silent indexing for chapter: %d", nextSpineIndex);
  }
}

void EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  const uint8_t percent = epubProgressPercentByte(*epub, spineIndex, currentPage, pageCount);
  if (!writeReaderProgressCache(epub->getCachePath(), spineIndex, currentPage, pageCount, percent)) {
    LOG_ERR("ERS", "Could not save progress!");
    return;
  }
  globalReadingSessionTracker().updateProgress(percent);
  LOG_DBG("ERS", "Progress saved: Chapter %d, Page %d (%d%%)", spineIndex, currentPage, percent);
}
void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  const auto t0 = millis();
  logReaderMemSnapshot("render_start");
  logIntegrityProbe("renderContents_entry");
  auto* fcm = renderer.getFontCacheManager();
  fcm->resetStats();

  const int viewportHeight = std::max(0, renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom);
  const int contentTop = orientedMarginTop + getImageOnlyPageYOffset(*page, viewportHeight);

  // Warm any missing image pixel caches BEFORE font prewarm and BW backup chunks
  // reduce heap contig below the ~60 KB the PNG/JPG decoder needs. The decode
  // writes pixels into the framebuffer as a side effect, so we reclear before
  // the real BW render begins. Skips when no decode is needed (all images cached
  // or the page is text-only). Mirrors the effectiveForceLoad rule used by the
  // BW render below so placeholder logic is identical.
  const bool warmForceLoad = forceLoadLargeImages || !SETTINGS.largeImagePlaceholder;
  page->warmImageCaches(renderer, orientedMarginLeft, contentTop, warmForceLoad);
  renderer.clearScreen();
  logIntegrityProbe("renderContents_after_warmImages");

  logReaderMemSnapshot("prewarm_begin");

  // Font prewarm: scan pass accumulates text, then prewarm, then real render
  const uint32_t heapBefore = esp_get_free_heap_size();
  const uint32_t contigBefore = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
  auto scope = fcm->createPrewarmScope();
  page->renderTextOnly(renderer, getEffectiveReaderFontId(), orientedMarginLeft, contentTop);  // scan pass
  scope.endScanAndPrewarm();
  const uint32_t heapAfter = esp_get_free_heap_size();
  const uint32_t contigAfter = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
  fcm->logStats("prewarm");
  const auto tPrewarm = millis();

  // contig= reports the largest contiguous block, which is what large allocations
  // (e.g. 60 KB PNG decoder) actually need. free=N with contig<<N means fragmentation.
  LOG_DBG("ERS", "Heap: before=%lu (contig=%lu) after=%lu (contig=%lu) delta=%ld", heapBefore, contigBefore, heapAfter,
          contigAfter, (int32_t)heapAfter - (int32_t)heapBefore);
  logReaderMemSnapshot("prewarm_end");
  logIntegrityProbe("renderContents_after_fontPrewarm");

  const bool aaConfigured = SETTINGS.textAntiAliasing;
  bool aaEnabledForThisRender = aaConfigured;
  if (aaConfigured && antiAliasingSuspendedLowMemory) {
    const uint32_t freeHeapNow = esp_get_free_heap_size();
    const uint32_t contigHeapNow = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
    if (freeHeapNow >= AA_RECOVERY_FREE_HEAP_BYTES && contigHeapNow >= AA_RECOVERY_CONTIG_HEAP_BYTES) {
      antiAliasingSuspendedLowMemory = false;
      LOG_INF("ERS", "Re-enabling text anti-aliasing after heap recovery (free=%lu contig=%lu)", freeHeapNow,
              contigHeapNow);
    } else {
      aaEnabledForThisRender = false;
    }
  }
  lastRenderStats.textAntiAliasing = aaEnabledForThisRender;

  const bool effectiveForceLoad = forceLoadLargeImages || !SETTINGS.largeImagePlaceholder;
  pageHasPlaceholders = page->hasPlaceholderImages(effectiveForceLoad);

  // Force special handling for pages with at least one real (decoded) image when anti-aliasing is on.
  // Mixed pages (some decoded, some placeholder) still need the AA codepath.
  bool imagePageWithAA =
      page->hasImages() && !page->allImagesArePlaceholders(effectiveForceLoad) && aaEnabledForThisRender;
  bool forceHalfRefreshThisPage = pendingHalfRefreshAfterImagePage && SETTINGS.halfRefreshAfterImagePage;
  pendingHalfRefreshAfterImagePage = false;
  lastRenderStats.imagePageWithAA = imagePageWithAA;
  lastRenderStats.forcedHalfRefresh = forceHalfRefreshThisPage;

  logReaderMemSnapshot("before_bw_render");
  page->render(renderer, getEffectiveReaderFontId(), orientedMarginLeft, contentTop, effectiveForceLoad);
  renderStatusBar();
  if (showTruncatedSectionHintThisRender) {
    const int hintX = orientedMarginLeft + 4;
    const int hintY1 = contentTop + 4;
    const int hintY2 = hintY1 + 20;
    const int maxWidth = std::max(0, renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight - 8);
    // Clear a dedicated band so the hint stays readable over any page content.
    const int boxX = hintX - 2;
    const int boxY = hintY1 - 2;
    const int boxW = maxWidth + 4;
    const int boxH = 44;
    renderer.fillRect(boxX, boxY, boxW, boxH, false);
    renderer.drawRect(boxX, boxY, boxW, boxH, true);
    const std::string line1 = renderer.truncatedText(UI_10_FONT_ID, TRUNCATED_SECTION_HINT_LINE_1, maxWidth);
    const std::string line2 = renderer.truncatedText(UI_10_FONT_ID, TRUNCATED_SECTION_HINT_LINE_2, maxWidth);
    renderer.drawText(UI_10_FONT_ID, hintX, hintY1, line1.c_str(), true, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, hintX, hintY2, line2.c_str(), true);
  }
  fcm->logStats("bw_render");
  const auto tBwRender = millis();
  logReaderMemSnapshot("after_bw_render");
  logIntegrityProbe("renderContents_after_bwRender");

  if (imagePageWithAA) {
    // Double FAST_REFRESH with selective image blanking (pablohc's technique):
    // HALF_REFRESH sets particles too firmly for the grayscale LUT to adjust.
    // Instead, blank only the image area and do two fast refreshes.
    // Step 1: Display page with image area blanked (text appears, image area white)
    // Step 2: Re-render with images and display again (images appear clean)
    int16_t imgX, imgY, imgW, imgH;
    if (page->getImageBoundingBox(imgX, imgY, imgW, imgH)) {
      renderer.fillRect(imgX + orientedMarginLeft, imgY + contentTop, imgW, imgH, false);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);

      // Re-render page content to restore images into the blanked area
      // Status bar is not re-rendered here to avoid reading stale dynamic values (e.g. battery %)
      page->render(renderer, getEffectiveReaderFontId(), orientedMarginLeft, contentTop, effectiveForceLoad);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    } else {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
    // Double FAST_REFRESH handles ghosting for image pages; don't count toward full refresh cadence
    if (forceHalfRefreshThisPage) {
      pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
    }
  } else if (forceHalfRefreshThisPage) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  }
  const auto tDisplay = millis();

  // Only schedule the half-refresh if at least one real image was decoded on this page.
  // Placeholder-only pages don't deposit grayscale data that needs settling.
  if (page->hasImages() && !page->allImagesArePlaceholders(effectiveForceLoad) &&
      getEffectiveImageRendering() != CrossPointSettings::IMAGES_SUPPRESS) {
    pendingHalfRefreshAfterImagePage = true;
  }

  // Tiled grayscale: try the strip path first when AA is on and the controller
  // supports it. It allocates an ~8 KB scratch instead of saving a partial BW
  // frame, leaving the live BW framebuffer intact (no storeBwBuffer needed).
  // Falls through to the legacy snapshot path if strip is unsupported or the
  // scratch can't be allocated.
  bool grayscaleDone = false;
  uint32_t tiledGrayMs = 0;
  if (aaEnabledForThisRender) {
    logReaderMemSnapshot("tiled_gray_begin");
    logIntegrityProbe("renderContents_before_tiledGray");
    const auto tTiledBegin = millis();
    grayscaleDone = runTiledGrayscalePass(renderer, *page, getEffectiveReaderFontId(), orientedMarginLeft, contentTop,
                                          SETTINGS.fastAntiAliasing);
    if (grayscaleDone) {
      tiledGrayMs = millis() - tTiledBegin;
      fcm->logStats("tiled_gray");
      logReaderMemSnapshot("tiled_gray_end");
      logIntegrityProbe("renderContents_after_tiledGray");
    }
  }

  if (grayscaleDone) {
    const auto tEnd = millis();
    lastRenderStats.usedGrayscale = true;
    lastRenderStats.phases = {tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, 0, tiledGrayMs, 0, 0, 0,
                              tEnd - t0};
    LOG_DBG("ERS", "Page render (tiled): prewarm=%lums bw_render=%lums display=%lums tiled_gray=%lums total=%lums",
            tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tiledGrayMs, tEnd - t0);
  } else {
    // Legacy fallback: save (partial) BW frame, render LSB+MSB planes into the
    // live framebuffer, display, then restore. Pre-check heap to avoid entering
    // the chunk-retry path when success is unlikely. Skip the snapshot entirely
    // when AA is off — there is nothing to restore from since no grayscale pass
    // will run.
    const uint32_t bwStoreFreeHeap = esp_get_free_heap_size();
    const uint32_t bwStoreContigHeap = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
    const bool shouldAttemptBwSnapshot = aaEnabledForThisRender && bwStoreFreeHeap >= BW_SNAPSHOT_MIN_FREE_HEAP_BYTES &&
                                         bwStoreContigHeap >= BW_SNAPSHOT_MIN_CONTIG_HEAP_BYTES;

    logReaderMemSnapshot("bw_store_begin");
    bool bwBufferStored = false;
    if (shouldAttemptBwSnapshot) {
      // Snapshot the full framebuffer. The AA pass calls clearScreen(0x00) which
      // touches every panel row, and restoreBwBuffer() ends with
      // cleanupGrayscaleBuffers(frameBuffer) that syncs the controller's belief
      // from the framebuffer — so any row not in the snapshot ends up zero in the
      // FB and the controller drifts out of sync with what's physically on the
      // panel, producing ghosting on the next page turn (issue #256).
      bwBufferStored = renderer.storeBwBuffer();
    } else if (aaEnabledForThisRender) {
      LOG_INF("ERS", "Skipping BW snapshot precheck (free=%lu contig=%lu, need free>=%lu contig>=%lu)", bwStoreFreeHeap,
              bwStoreContigHeap, static_cast<uint32_t>(BW_SNAPSHOT_MIN_FREE_HEAP_BYTES),
              static_cast<uint32_t>(BW_SNAPSHOT_MIN_CONTIG_HEAP_BYTES));
    }
    const auto tBwStore = millis();
    logReaderMemSnapshot("bw_store_end");
    if (aaEnabledForThisRender && !bwBufferStored) {
      if (!antiAliasingSuspendedLowMemory) {
        antiAliasingSuspendedLowMemory = true;
        const uint32_t freeHeapNow = esp_get_free_heap_size();
        const uint32_t contigHeapNow = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
        LOG_INF("ERS", "Suspending text anti-aliasing due to low heap (free=%lu contig=%lu)", freeHeapNow,
                contigHeapNow);
      }
      LOG_INF("ERS", "Skipping grayscale/BW-restore for this page (insufficient heap for BW snapshot)");
    }

    // grayscale rendering
    // TODO: Only do this if font supports it
    if (aaEnabledForThisRender && bwBufferStored) {
      // Push fast-AA toggle into the SDK before the AA refresh (X3 only; no-op
      // on X4). Mirrors what runTiledGrayscalePass() does.
      renderer.setFastGrayscaleLut(SETTINGS.fastAntiAliasing);
      logReaderMemSnapshot("gray_lsb_begin");
      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      page->renderTextOnly(renderer, getEffectiveReaderFontId(), orientedMarginLeft, contentTop);
      renderer.copyGrayscaleLsbBuffers();
      const auto tGrayLsb = millis();
      logReaderMemSnapshot("gray_lsb_end");

      // Render and copy to MSB buffer
      logReaderMemSnapshot("gray_msb_begin");
      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      page->renderTextOnly(renderer, getEffectiveReaderFontId(), orientedMarginLeft, contentTop);
      renderer.copyGrayscaleMsbBuffers();
      const auto tGrayMsb = millis();
      logReaderMemSnapshot("gray_msb_end");

      // display grayscale part
      logReaderMemSnapshot("gray_display_begin");
      renderer.displayGrayBuffer();
      const auto tGrayDisplay = millis();
      renderer.setRenderMode(GfxRenderer::BW);
      fcm->logStats("gray");
      logReaderMemSnapshot("gray_display_end");

      // restore the bw data
      logReaderMemSnapshot("bw_restore_begin");
      renderer.restoreBwBuffer();
      const auto tBwRestore = millis();
      logReaderMemSnapshot("bw_restore_end");

      const auto tEnd = millis();
      lastRenderStats.usedGrayscale = true;
      lastRenderStats.phases = {tPrewarm - t0,           tBwRender - tPrewarm,      tDisplay - tBwRender,
                                tBwStore - tDisplay,     tGrayLsb - tBwStore,       tGrayMsb - tGrayLsb,
                                tGrayDisplay - tGrayMsb, tBwRestore - tGrayDisplay, tEnd - t0};
      LOG_DBG("ERS",
              "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums "
              "gray_lsb=%lums gray_msb=%lums gray_display=%lums bw_restore=%lums total=%lums",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, tGrayLsb - tBwStore,
              tGrayMsb - tGrayLsb, tGrayDisplay - tGrayMsb, tBwRestore - tGrayDisplay, tEnd - t0);
    } else {
      uint32_t bwRestoreMs = 0;
      if (bwBufferStored) {
        // restore the bw data
        logReaderMemSnapshot("bw_restore_begin");
        renderer.restoreBwBuffer();
        const auto tBwRestore = millis();
        logReaderMemSnapshot("bw_restore_end");
        bwRestoreMs = tBwRestore - tBwStore;
      }

      const auto tEnd = millis();
      lastRenderStats.usedGrayscale = false;
      lastRenderStats.phases = {
          tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, 0, 0, 0, bwRestoreMs,
          tEnd - t0};
      LOG_DBG("ERS",
              "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums bw_restore=%lums total=%lums",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, bwRestoreMs, tEnd - t0);
    }
  }

  if (const auto* cacheManager = renderer.getFontCacheManager()) {
    if (const auto* decompressor = cacheManager->getDecompressor()) {
      const auto& stats = decompressor->getStats();
      lastRenderStats.fontCacheHits = stats.cacheHits;
      lastRenderStats.fontCacheMisses = stats.cacheMisses;
      lastRenderStats.fontDecompressMs = stats.decompressTimeMs;
      lastRenderStats.fontUniqueGroups = stats.uniqueGroupsAccessed;
      lastRenderStats.fontPageBufferBytes = stats.pageBufferBytes;
      lastRenderStats.fontPageGlyphsBytes = stats.pageGlyphsBytes;
      lastRenderStats.fontPeakTempBytes = stats.peakTempBytes;
      lastRenderStats.fontGetBitmapTimeUs = stats.getBitmapTimeUs;
      lastRenderStats.fontGetBitmapCalls = stats.getBitmapCalls;
    }
  }
}

void EpubReaderActivity::renderPageContentOnly(const Page& page, const int orientedMarginTop,
                                               const int orientedMarginRight, const int orientedMarginBottom,
                                               const int orientedMarginLeft) {
  auto* fcm = renderer.getFontCacheManager();
  fcm->resetStats();

  const int viewportHeight = std::max(0, renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom);
  const int contentTop = orientedMarginTop + getImageOnlyPageYOffset(page, viewportHeight);

  auto scope = fcm->createPrewarmScope();
  page.renderTextOnly(renderer, getEffectiveReaderFontId(), orientedMarginLeft, contentTop);
  scope.endScanAndPrewarm();

  renderer.clearScreen();
  page.render(renderer, getEffectiveReaderFontId(), orientedMarginLeft, contentTop);
  // Status bar intentionally omitted — superimposed at display time with live values.
}

void EpubReaderActivity::displayPreRenderedPage(const Page& page, const int orientedMarginTop,
                                                const int orientedMarginRight, const int orientedMarginBottom,
                                                const int orientedMarginLeft) {
  const int viewportHeight = std::max(0, renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom);
  const int contentTop = orientedMarginTop + getImageOnlyPageYOffset(page, viewportHeight);

  renderStatusBar();

  // Pre-rendered pages are text-only (image pages are excluded from pre-rendering), so we
  // always go through the normal refresh cycle — no imagePageWithAA or forceHalfRefresh paths.
  const bool forceHalfRefreshThisPage = pendingHalfRefreshAfterImagePage && SETTINGS.halfRefreshAfterImagePage;
  pendingHalfRefreshAfterImagePage = false;
  if (forceHalfRefreshThisPage) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  }

  // Grayscale AA pass. Prefer the tiled strip path (no BW snapshot needed);
  // fall back to the legacy storeBwBufferRect path on controllers that don't
  // support strip grayscale or when the strip scratch can't be allocated.
  const bool aaConfigured = SETTINGS.textAntiAliasing && !antiAliasingSuspendedLowMemory;
  if (aaConfigured) {
    if (runTiledGrayscalePass(renderer, page, getEffectiveReaderFontId(), orientedMarginLeft, contentTop,
                              SETTINGS.fastAntiAliasing)) {
      return;
    }

    const uint32_t freeHeap = esp_get_free_heap_size();
    const uint32_t contigHeap = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
    if (freeHeap >= BW_SNAPSHOT_MIN_FREE_HEAP_BYTES && contigHeap >= BW_SNAPSHOT_MIN_CONTIG_HEAP_BYTES) {
      // Full-framebuffer snapshot — see the matching site above for the rationale (issue #256).
      if (renderer.storeBwBuffer()) {
        renderer.setFastGrayscaleLut(SETTINGS.fastAntiAliasing);
        renderer.clearScreen(0x00);
        renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
        page.renderTextOnly(renderer, getEffectiveReaderFontId(), orientedMarginLeft, contentTop);
        renderer.copyGrayscaleLsbBuffers();

        renderer.clearScreen(0x00);
        renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
        page.renderTextOnly(renderer, getEffectiveReaderFontId(), orientedMarginLeft, contentTop);
        renderer.copyGrayscaleMsbBuffers();

        renderer.displayGrayBuffer();
        renderer.setRenderMode(GfxRenderer::BW);
        renderer.restoreBwBuffer();
      }
    }
  }
}

void EpubReaderActivity::restoreCurrentPageToBufferIfPreRendered() {
  if (!preRenderedPage.ready || !section || !epub) {
    return;
  }

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  const int statusBarTopHeight = UITheme::getStatusBarTopHeight(automaticPageTurnActive);
  const int statusBarBottomHeight = UITheme::getStatusBarBottomHeight(automaticPageTurnActive);
  orientedMarginTop += std::max(static_cast<int>(SETTINGS.screenMargin), statusBarTopHeight);
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;
  orientedMarginBottom += std::max(static_cast<int>(SETTINGS.screenMargin), statusBarBottomHeight);

  auto p = section->loadPageFromSectionFile();
  if (!p) {
    return;
  }
  renderPageContentOnly(*p, orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
  preRenderedPage.ready = false;
  pendingPreRender = false;
  usePreRenderedBuffer = false;
}

void EpubReaderActivity::renderStatusBar() const {
  // Calculate progress in book
  const int currentPage = section->currentPage + 1;
  const float pageCount = section->pageCount;
  const float sectionChapterProg = (pageCount > 0) ? (static_cast<float>(currentPage) / pageCount) : 0;
  const float bookProgress = epub->calculateProgress(currentSpineIndex, sectionChapterProg) * 100;

  std::string title;

  if (automaticPageTurnActive) {
    title = tr(STR_AUTO_TURN_ENABLED) + std::to_string(60 * 1000 / pageTurnDuration);
  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    const int tocIndex =
        section ? section->getTocIndexForPage(section->currentPage) : epub->getTocIndexForSpineIndex(currentSpineIndex);
    if (tocIndex == -1) {
      title = tr(STR_UNNAMED);
    } else {
      const auto tocItem = epub->getTocItem(tocIndex);
      title = tocItem.title;
    }
  } else if (SETTINGS.statusBarTitle == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = epub->getTitle();
  }

  const bool isStarred = section && bookmarkStore.has(static_cast<uint16_t>(currentSpineIndex),
                                                      static_cast<uint16_t>(section->currentPage));
  std::string printedPageLabel;
  if (section) {
    if (const auto label = section->getPrintedPageLabelForPage(static_cast<uint16_t>(section->currentPage))) {
      printedPageLabel = *label;
    }
  }
  GUI.drawStatusBar(renderer, bookProgress, currentPage, pageCount, title, 0, isStarred, printedPageLabel);

  lastStatusBarPage = currentPage;
  lastStatusBarBattery = SETTINGS.statusBarBattery ? static_cast<int>(powerManager.getBatteryPercentage()) : -1;
  if (SETTINGS.useClock && SETTINGS.statusBarClock && HalClock::isSynced()) {
    const time_t now = HalClock::now();
    lastStatusBarClockMinute = now > 0 ? static_cast<int>(now / 60) : -1;
  } else {
    lastStatusBarClockMinute = -1;
  }
}

bool EpubReaderActivity::shouldSkipPeriodicUpdate() const {
  if (lastStatusBarPage < 0) return false;  // no baseline yet — let the first render happen
  const int currentPage = section ? section->currentPage + 1 : -1;
  if (currentPage != lastStatusBarPage) return false;
  if (SETTINGS.statusBarBattery) {
    if (static_cast<int>(powerManager.getBatteryPercentage()) != lastStatusBarBattery) return false;
  }
  if (SETTINGS.useClock && SETTINGS.statusBarClock && HalClock::isSynced()) {
    const time_t now = HalClock::now();
    const int minute = now > 0 ? static_cast<int>(now / 60) : -1;
    if (minute != lastStatusBarClockMinute) return false;
  }
  return true;
}

void EpubReaderActivity::navigateToHref(const std::string& hrefStr, const bool savePosition) {
  if (!epub) return;

  // Push current position onto saved stack
  if (savePosition && section && footnoteDepth < MAX_FOOTNOTE_DEPTH) {
    savedPositions[footnoteDepth] = {currentSpineIndex, section->currentPage};
    footnoteDepth++;
    LOG_DBG("ERS", "Saved position [%d]: spine %d, page %d", footnoteDepth, currentSpineIndex, section->currentPage);
  }

  // Extract fragment anchor (e.g. "#note1" or "chapter2.xhtml#note1")
  std::string anchor;
  const auto hashPos = hrefStr.find('#');
  if (hashPos != std::string::npos && hashPos + 1 < hrefStr.size()) {
    anchor = hrefStr.substr(hashPos + 1);
  }

  // Check for same-file anchor reference (#anchor only)
  bool sameFile = !hrefStr.empty() && hrefStr[0] == '#';

  int targetSpineIndex;
  if (sameFile) {
    targetSpineIndex = currentSpineIndex;
  } else {
    targetSpineIndex = epub->resolveHrefToSpineIndex(hrefStr);
  }

  if (targetSpineIndex < 0) {
    LOG_DBG("ERS", "Could not resolve href: %s", hrefStr.c_str());
    if (savePosition && footnoteDepth > 0) footnoteDepth--;  // undo push
    return;
  }

  {
    RenderLock lock(*this);
    navTarget = anchor.empty() ? NavigationTarget::makePage(0) : NavigationTarget::makeAnchor(std::move(anchor));
    currentSpineIndex = targetSpineIndex;
    section.reset();
  }
  requestUpdate();
  LOG_DBG("ERS", "Navigated to spine %d for href: %s", targetSpineIndex, hrefStr.c_str());
}

void EpubReaderActivity::restoreSavedPosition() {
  if (footnoteDepth <= 0) return;
  footnoteDepth--;
  const auto& pos = savedPositions[footnoteDepth];
  LOG_DBG("ERS", "Restoring position [%d]: spine %d, page %d", footnoteDepth, pos.spineIndex, pos.pageNumber);

  {
    RenderLock lock(*this);
    currentSpineIndex = pos.spineIndex;
    navTarget = NavigationTarget::makePage(pos.pageNumber);
    section.reset();
  }
  requestUpdate();
}

bool EpubReaderActivity::drawCurrentPageToBuffer(const std::string& filePath, GfxRenderer& renderer) {
  auto epub = std::make_shared<Epub>(filePath, "/.crosspoint");
  // Load CSS when embeddedStyle is enabled, as createSectionFile may need it to rebuild the cache.
  if (!epub->load(true, SETTINGS.embeddedStyle == 0)) {
    LOG_DBG("SLP", "EPUB: failed to load %s", filePath.c_str());
    return false;
  }

  epub->setupCacheDir();

  // Load saved spine index and page number
  int spineIndex = 0, pageNumber = 0;
  FsFile f;
  if (Storage.openFileForRead("SLP", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    if (f.read(data, 6) == 6) {
      spineIndex = (int)((uint32_t)data[0] | ((uint32_t)data[1] << 8));
      pageNumber = (int)((uint32_t)data[2] | ((uint32_t)data[3] << 8));
    }
    f.close();
  }
  if (spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) spineIndex = 0;

  // Apply the reader orientation so margins match what the reader would produce
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  // Compute margins exactly as render() does
  int marginTop, marginRight, marginBottom, marginLeft;
  renderer.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);
  marginTop += std::max(static_cast<int>(SETTINGS.screenMargin), UITheme::getStatusBarTopHeight());
  marginLeft += SETTINGS.screenMargin;
  marginRight += SETTINGS.screenMargin;
  marginBottom += std::max(static_cast<int>(SETTINGS.screenMargin), UITheme::getStatusBarBottomHeight());

  const uint16_t viewportWidth = renderer.getScreenWidth() - marginLeft - marginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - marginTop - marginBottom;

  // Load or rebuild the section cache. Rebuilding is needed when the cache is missing or stale
  // (e.g. after a firmware update). A no-op popup callback avoids any UI during sleep preparation.
  const RecentBook currentBook = RECENT_BOOKS.getBookByPath(filePath);
  const bool hasLocalSdOverride = !currentBook.sdFontFamilyOverride.empty();
  const uint8_t effectiveFontFamily =
      currentBook.fontFamilyOverride >= 0 ? static_cast<uint8_t>(currentBook.fontFamilyOverride) : SETTINGS.fontFamily;
  const uint8_t effectiveFontSize =
      currentBook.fontSizeOverride >= 0 ? static_cast<uint8_t>(currentBook.fontSizeOverride) : SETTINGS.fontSize;
  int effectiveFontId = 0;
  if (hasLocalSdOverride) {
    effectiveFontId = resolveSdCardFontId(currentBook.sdFontFamilyOverride.c_str(), effectiveFontSize);
  }
  if (effectiveFontId == 0 && currentBook.fontFamilyOverride >= 0) {
    effectiveFontId = CrossPointSettings::getBuiltinReaderFontId(effectiveFontFamily, effectiveFontSize);
  }
  if (effectiveFontId == 0 && currentBook.fontSizeOverride >= 0 && SETTINGS.sdFontFamilyName[0] != '\0') {
    effectiveFontId = resolveSdCardFontId(SETTINGS.sdFontFamilyName, effectiveFontSize);
  }
  if (effectiveFontId == 0 && currentBook.fontSizeOverride >= 0) {
    effectiveFontId = CrossPointSettings::getBuiltinReaderFontId(SETTINGS.fontFamily, effectiveFontSize);
  }
  if (effectiveFontId == 0) {
    effectiveFontId = SETTINGS.getReaderFontId();
  }
  const auto getEffectiveLineCompression = [&](int fontId) {
    const int notosansId = CrossPointSettings::getBuiltinReaderFontId(CrossPointSettings::NOTOSANS, effectiveFontSize);

    if (fontId == notosansId) {
      switch (SETTINGS.lineSpacing) {
        case CrossPointSettings::TIGHT:
          return 0.90f;
        case CrossPointSettings::NORMAL:
        default:
          return 0.95f;
        case CrossPointSettings::WIDE:
          return 1.0f;
      }
    }

    switch (SETTINGS.lineSpacing) {
      case CrossPointSettings::TIGHT:
        return 0.95f;
      case CrossPointSettings::NORMAL:
      default:
        return 1.0f;
      case CrossPointSettings::WIDE:
        return 1.1f;
    }
  };

  const float effectiveLineCompression = getEffectiveLineCompression(effectiveFontId);
  auto section = std::make_unique<Section>(epub, spineIndex, renderer);
  if (!section->loadSectionFile(effectiveFontId, effectiveLineCompression, SETTINGS.extraParagraphSpacing,
                                SETTINGS.paragraphAlignment, viewportWidth, viewportHeight, SETTINGS.hyphenationEnabled,
                                SETTINGS.embeddedStyle, static_cast<bool>(SETTINGS.bionicReading),
                                SETTINGS.imageRendering)) {
    LOG_DBG("SLP", "EPUB: section cache not found for spine %d, rebuilding", spineIndex);
    if (!section->createSectionFile(effectiveFontId, effectiveLineCompression, SETTINGS.extraParagraphSpacing,
                                    SETTINGS.paragraphAlignment, viewportWidth, viewportHeight,
                                    SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle,
                                    static_cast<bool>(SETTINGS.bionicReading), SETTINGS.imageRendering)) {
      LOG_ERR("SLP", "EPUB: failed to rebuild section cache for spine %d", spineIndex);
      return false;
    }
  }

  if (pageNumber < 0 || pageNumber >= section->pageCount) pageNumber = 0;
  section->currentPage = pageNumber;

  auto page = section->loadPageFromSectionFile();
  if (!page) {
    LOG_DBG("SLP", "EPUB: failed to load page %d", pageNumber);
    return false;
  }

  const int imageOnlyOffset = getImageOnlyPageYOffset(*page, viewportHeight);
  const int renderMarginTop = marginTop + imageOnlyOffset;
  renderer.clearScreen();
  page->render(renderer, effectiveFontId, marginLeft, renderMarginTop);
  // No displayBuffer call — caller (SleepActivity) handles that after compositing the overlay
  return true;
}

void EpubReaderActivity::openReaderMenu() {
  const int currentPage = section ? section->currentPage + 1 : 0;
  const int totalPages = section ? section->pageCount : 0;

  if (!epub) {
    return;
  }

  float bookProgress = 0.0f;
  if (epub->getBookSize() > 0 && section && section->pageCount > 0) {
    const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(section->pageCount);
    bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
  }
  const int bookProgressPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
  const bool isCurrentPageStarred = section && bookmarkStore.has(static_cast<uint16_t>(currentSpineIndex),
                                                                 static_cast<uint16_t>(section->currentPage));

  // Show the "Go to printed page" item only when this book has at least one integer-labelled
  // entry in pagelist.bin. Roman-only or empty page lists are excluded — the numeric input
  // dialog can't address them anyway.
  const auto printedPageList = epub->loadPrintedPageList();
  const bool hasPrintedPages = std::any_of(printedPageList.begin(), printedPageList.end(), [](const auto& entry) {
    return parsePrintedPageLabel(entry.label).has_value();
  });

  ReaderUtils::enforceExitFullRefresh(renderer);
  startActivityForResult(
      std::make_unique<EpubReaderMenuActivity>(
          renderer, mappedInput, epub->getTitle(), currentPage, totalPages, bookProgressPercent, SETTINGS.orientation,
          !currentPageFootnotes.empty(), bookEmbeddedStyleOverride, bookImageRenderingOverride, bookFontFamilyOverride,
          bookSdFontFamilyOverride, bookFontSizeOverride, SETTINGS.textDarkness, bookBionicReadingOverride,
          bookParagraphAlignmentOverride, !bookmarkStore.isEmpty(), isCurrentPageStarred, hasPrintedPages),
      [this](const ActivityResult& result) {
        const auto& menu = std::get<MenuResult>(result.data);
        applyOrientation(menu.orientation);
        applyTextDarkness(menu.textDarkness);
        toggleAutoPageTurn(menu.pageTurnOption);
        applyBookReaderOverrides(menu.embeddedStyleOverride, menu.imageRenderingOverride, menu.fontFamilyOverride,
                                 menu.sdFontFamilyOverride, menu.fontSizeOverride,
                                 static_cast<bool>(menu.bionicReadingOverride), menu.paragraphAlignmentOverride);
        if (!result.isCancelled) {
          onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(menu.action));
        }
      });
}

void EpubReaderActivity::onButtonAction(const CrossPointSettings::BUTTON_ACTION action) {
  using BA = CrossPointSettings::BUTTON_ACTION;
  switch (action) {
    case BA::BTN_PAGE_FORWARD:
      pageTurn(true);
      break;
    case BA::BTN_PAGE_BACK:
      pageTurn(false);
      break;
    case BA::BTN_PAGE_FORWARD_10:
      for (int i = 0; i < 10; i++) {
        if (!stepPageState(true)) break;
      }
      requestUpdate();
      break;
    case BA::BTN_PAGE_BACK_10:
      for (int i = 0; i < 10; i++) {
        if (!stepPageState(false)) break;
      }
      requestUpdate();
      break;
    case BA::BTN_STAR_PAGE:
      if (section) {
        bookmarkStore.toggle(static_cast<uint16_t>(currentSpineIndex), static_cast<uint16_t>(section->currentPage));
        requestUpdate();
      }
      break;
    case BA::BTN_FOOTNOTES:
      if (!currentPageFootnotes.empty()) {
        if (currentPageFootnotes.size() == 1) {
          navigateToHref(currentPageFootnotes[0].href, true);
        } else {
          startActivityForResult(
              std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
              [this](const ActivityResult& result) {
                if (!result.isCancelled) {
                  const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                  navigateToHref(footnoteResult.href, true);
                }
              });
        }
      }
      break;
    case BA::BTN_OPEN_TOC:
      if (epub) {
        const int spineIdx = currentSpineIndex;
        const int tocIdx = section ? section->getTocIndexForPage(section->currentPage)
                                   : epub->getTocIndexForSpineIndex(currentSpineIndex);
        ReaderUtils::enforceExitFullRefresh(renderer);
        startActivityForResult(std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub,
                                                                                    epub->getPath(), spineIdx, tocIdx),
                               [this](const ActivityResult& result) {
                                 if (result.isCancelled) return;
                                 RenderLock lock(*this);
                                 const auto& chapter = std::get<ChapterResult>(result.data);
                                 auto resolvedPage =
                                     (chapter.tocIndex && chapter.spineIndex == currentSpineIndex && section)
                                         ? section->getPageForTocIndex(*chapter.tocIndex)
                                         : std::nullopt;
                                 if (resolvedPage) {
                                   section->currentPage = *resolvedPage;
                                   forceLoadLargeImages = false;
                                   pageHasPlaceholders = false;
                                 } else {
                                   navTarget = chapter.tocIndex ? NavigationTarget::makeTocIndex(*chapter.tocIndex)
                                                                : NavigationTarget::makePage(0);
                                   currentSpineIndex = chapter.spineIndex;
                                   section.reset();
                                 }
                               });
      }
      break;
    case BA::BTN_NEXT_SECTION:
    case BA::BTN_PREV_SECTION: {
      const bool forward = (action == BA::BTN_NEXT_SECTION);
      {
        RenderLock lock(*this);
        if (section && section->pageCount > 0) {
          const int curTocIndex = section->getTocIndexForPage(section->currentPage);
          const int nextTocIndex = forward ? curTocIndex + 1 : curTocIndex - 1;
          if (curTocIndex < 0) {
            navTarget = NavigationTarget::makePage(0);
            currentSpineIndex = forward ? currentSpineIndex + 1 : currentSpineIndex - 1;
            section.reset();
          } else if (nextTocIndex >= 0 && nextTocIndex < epub->getTocItemsCount()) {
            const int newSpineIndex = epub->getSpineIndexForTocIndex(nextTocIndex);
            if (newSpineIndex == currentSpineIndex) {
              if (const auto resolvedPage = section->getPageForTocIndex(nextTocIndex)) {
                section->currentPage = *resolvedPage;
                forceLoadLargeImages = false;
                pageHasPlaceholders = false;
              }
            } else {
              navTarget = NavigationTarget::makeTocIndex(nextTocIndex);
              currentSpineIndex = newSpineIndex;
              section.reset();
            }
          } else if (forward) {
            navTarget = NavigationTarget::makePage(0);
            currentSpineIndex = epub->getSpineItemsCount();
            section.reset();
          } else {
            navTarget = NavigationTarget::makePage(0);
            currentSpineIndex = epub->getTocItem(curTocIndex).spineIndex - 1;
            section.reset();
          }
        } else {
          navTarget = NavigationTarget::makePage(0);
          currentSpineIndex = forward ? currentSpineIndex + 1 : currentSpineIndex - 1;
          section.reset();
        }
      }
      requestUpdate();
      break;
    }
    case BA::BTN_EXIT_READER:
      ReaderUtils::enforceExitFullRefresh(renderer);
      if (tryAutoPushOnClose()) break;
      finish();
      break;
    case BA::BTN_READER_MENU:
      if (epub) {
        openReaderMenu();
      }
      break;
    case BA::BTN_TOGGLE_BIONIC_READING:
      if (epub) {
        applyBookReaderOverrides(bookEmbeddedStyleOverride, bookImageRenderingOverride, bookFontFamilyOverride,
                                 bookSdFontFamilyOverride, bookFontSizeOverride, !bookBionicReadingOverride,
                                 bookParagraphAlignmentOverride);
        requestUpdate();
      }
      break;
    case BA::BTN_CYCLE_FONT_SIZE:
      if (epub) {
        const uint8_t current =
            (bookFontSizeOverride >= 0) ? static_cast<uint8_t>(bookFontSizeOverride) : SETTINGS.fontSize;
        const int8_t next = static_cast<int8_t>((current + 1) % CrossPointSettings::FONT_SIZE_COUNT);
        applyBookReaderOverrides(bookEmbeddedStyleOverride, bookImageRenderingOverride, bookFontFamilyOverride,
                                 bookSdFontFamilyOverride, next, bookBionicReadingOverride,
                                 bookParagraphAlignmentOverride);
        requestUpdate();
      }
      break;
    case BA::BTN_CYCLE_ORIENTATION:
      if (epub) {
        const uint8_t nextOrientation =
            static_cast<uint8_t>((SETTINGS.orientation + 1) % CrossPointSettings::ORIENTATION_COUNT);
        applyOrientation(nextOrientation);
        requestUpdate();
      }
      break;
    case BA::BTN_KOREADER_SYNC:
      launchKOReaderSync(SyncLaunchMode::COMPARE);
      break;
    default:
      break;
  }
}
