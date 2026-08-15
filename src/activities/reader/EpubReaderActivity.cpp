#include "EpubReaderActivity.h"

#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#ifdef CROSSPOINT_BG_IMAGE_DECODE
#include <Epub/converters/ImageToFramebufferDecoder.h>  // pre-decode abort flag
#endif
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_system.h>

#include <algorithm>
#include <functional>
#include <iterator>
#include <limits>

#include "../../util/BookmarkFile.h"
#include "BookmarkEntry.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "DictionaryWordSelectActivity.h"
#include "EpubReaderBookmarksActivity.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderFootnotesActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "EpubReaderUtils.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "MappedInputManager.h"
#include "ProgressMapper.h"
#include "QrDisplayActivity.h"
#include "ReaderActivity.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "activities/settings/TextSettingsActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookmarkUtil.h"
#include "util/ScreenshotUtil.h"

namespace {
constexpr int PAGE_TURN_RATES[] = {1, 1, 3, 6, 12};
constexpr size_t initialBookmarkCacheCapacity = 16;
constexpr float bookmarkProgressEpsilon = 0.0001f;

int clampPercent(int percent) {
  if (percent < 0) {
    return 0;
  }
  if (percent > 100) {
    return 100;
  }
  return percent;
}

constexpr char READ_FOLDER[] = "/read";

bool isInReadFolder(const std::string& path) {
  constexpr size_t n = sizeof(READ_FOLDER) - 1;
  return path.size() > n && path.compare(0, n, READ_FOLDER) == 0 && path[n] == '/';
}

struct ProgressRange {
  float start;
  float end;
};

ProgressRange getPageProgressRange(const std::shared_ptr<Epub>& epub, const int spineIndex, const int page,
                                   const int pageCount) {
  if (pageCount <= 1) {
    return {epub->calculateProgress(spineIndex, 0.0f), epub->calculateProgress(spineIndex, 1.0f)};
  }

  const float step = 1.0f / static_cast<float>(pageCount - 1);
  const float anchor = std::clamp(static_cast<float>(page) * step, 0.0f, 1.0f);
  const float start = std::max(0.0f, anchor - (step * 0.5f));
  const float end = std::min(1.0f, anchor + (step * 0.5f));
  return {epub->calculateProgress(spineIndex, start), epub->calculateProgress(spineIndex, end)};
}

bool bookmarkMatchesProgress(const BookmarkEntry& bookmark, const int spineIndex, const int page, const int pageCount,
                             const ProgressRange& pageRange) {
  if (bookmark.computedSpineIndex == spineIndex && bookmark.computedChapterPageCount == pageCount &&
      bookmark.computedChapterProgress == page) {
    return true;
  }

  const float bookmarkProgress = std::clamp(bookmark.percentage, 0.0f, 1.0f);
  return bookmarkProgress + bookmarkProgressEpsilon >= pageRange.start &&
         bookmarkProgress - bookmarkProgressEpsilon <= pageRange.end;
}

std::string buildReadFolderDestination(const std::string& srcPath) {
  const size_t lastSlash = srcPath.rfind('/');
  const std::string filename = (lastSlash != std::string::npos) ? srcPath.substr(lastSlash + 1) : srcPath;

  Storage.mkdir(READ_FOLDER);
  std::string dstPath = std::string(READ_FOLDER) + "/" + filename;
  if (!Storage.exists(dstPath.c_str())) {
    return dstPath;
  }

  const size_t dotPos = filename.rfind('.');
  const std::string base = (dotPos != std::string::npos) ? filename.substr(0, dotPos) : filename;
  const std::string ext = (dotPos != std::string::npos) ? filename.substr(dotPos) : "";
  int suffix = 2;
  do {
    dstPath = std::string(READ_FOLDER) + "/" + base + " (" + std::to_string(suffix) + ")" + ext;
    suffix++;
  } while (Storage.exists(dstPath.c_str()) && suffix < 100);
  return dstPath;
}

void moveFinishedBookToReadFolder(const std::string& srcPath, const std::string& dstPath,
                                  const std::string& oldCachePath) {
  LOG_INF("ERS", "Moving finished epub: %s -> %s", srcPath.c_str(), dstPath.c_str());
  if (!Storage.rename(srcPath.c_str(), dstPath.c_str())) {
    LOG_ERR("ERS", "Failed to move finished book to '/Read' folder");
    return;
  }

  const std::string newCachePath = "/.crosspoint/epub_" + std::to_string(std::hash<std::string>{}(dstPath));
  if (!oldCachePath.empty() && Storage.exists(oldCachePath.c_str())) {
    if (!Storage.rename(oldCachePath.c_str(), newCachePath.c_str())) {
      LOG_ERR("ERS", "Failed to rename cache dir %s -> %s (non-fatal)", oldCachePath.c_str(), newCachePath.c_str());
    }
  }

  RECENT_BOOKS.updatePath(srcPath, dstPath, oldCachePath, newCachePath);
  if (APP_STATE.openEpubPath == srcPath) {
    APP_STATE.openEpubPath = dstPath;
    APP_STATE.saveToFile();
  }
}

}  // namespace

EpubReaderActivity::~EpubReaderActivity() {
  ImageBlock::setExtractor(nullptr, nullptr);

  if (footnoteDepth > 0 && epub) {
    const SavedPosition& origin = savedPositions[0];
    saveProgress(origin.spineIndex, origin.pageNumber, 0);
  }

  section.reset();
  if (pendingReadFolderMove && epub) {
    const std::string srcPath = epub->getPath();
    const std::string oldCachePath = epub->getCachePath();
    const std::string dstPath = buildReadFolderDestination(srcPath);
    epub.reset();
    moveFinishedBookToReadFolder(srcPath, dstPath, oldCachePath);
  } else {
    epub.reset();
  }
}

#ifdef CROSSPOINT_BG_BUILD_TASK
void EpubReaderActivity::onEnter() {
  ReaderActivity::onEnter();
  // Only with a book: a failed load has already called finish(), and the task
  // has nothing to drive.
  if (epub) startBgBuildTask();
}

void EpubReaderActivity::onExit() {
  // Stop the build task before anything else (it dereferences `section` and the
  // epub, both of which this activity's teardown drops); the join is bounded by
  // the task's short wait cadence.
  stopBgBuildTask();
#ifdef CROSSPOINT_NEXT_SECTION_PREBUILD
  // Task is stopped; safe to discard directly. The parked Section never has a
  // build context, so its destructor touches no shared build state.
  prebuiltSection.reset();
  prebuiltSpineIndex = -1;
#endif
  ReaderActivity::onExit();
}
#endif

bool EpubReaderActivity::loadBook() {
  auto loadedEpub = makeUniqueNoThrow<Epub>(bookPath, "/.crosspoint");
  if (!loadedEpub) {
    LOG_ERR("ERS", "Failed to allocate EPUB object");
    return false;
  }

  const bool uncached = !Storage.exists((loadedEpub->getCachePath() + "/book.bin").c_str());
  if (uncached) {
    disableFastInitialRefresh();
    GUI.drawPopup(renderer, tr(STR_INDEXING));
  }

  bool loaded;
  {
    std::optional<GfxRenderer::FrameBufferLoan> loan;
    if (uncached) loan.emplace(renderer);
    loaded = loadedEpub->load(true, SETTINGS.embeddedStyle == 0);
  }
  if (!loaded) {
    LOG_ERR("ERS", "Failed to load EPUB");
    return false;
  }
  epub = std::move(loadedEpub);

  ImageBlock::clearSessionRenderFailures();
  ImageBlock::setExtractor(epub.get(), [](void* ctx, const char* src, const char* dest) {
    return static_cast<Epub*>(ctx)->extractItemToFile(src, dest);
  });

  epub->setupCacheDir();

  HalFile f;
  if (Storage.openFileForRead("ERS", epub->getCachePath() + "/progress.bin", f)) {
    uint8_t data[10];
    int dataSize = f.read(data, sizeof(data));
    if (dataSize == 4 || dataSize == 6 || dataSize == 10) {
      currentSpineIndex = data[0] + (data[1] << 8);
      nextPageNumber = data[2] + (data[3] << 8);
      if (nextPageNumber == UINT16_MAX) {
        LOG_DBG("ERS", "Ignoring stale last-page sentinel from progress cache");
        nextPageNumber = 0;
      }
      cachedSpineIndex = currentSpineIndex;
      LOG_DBG("ERS", "Loaded cache: %d, %d", currentSpineIndex, nextPageNumber);
    }
    if (dataSize == 6) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
    } else if (dataSize == 10) {
      cachedChapterTotalPageCount = data[4] + (data[5] << 8);
      cachedVisibleTextOffset = static_cast<uint32_t>(data[6]) | (static_cast<uint32_t>(data[7]) << 8) |
                                (static_cast<uint32_t>(data[8]) << 16) | (static_cast<uint32_t>(data[9]) << 24);
    }
  }

  if (currentSpineIndex == 0) {
    int textSpineIndex = epub->getSpineIndexForTextReference();
    if (textSpineIndex != 0) {
      currentSpineIndex = textSpineIndex;
      cachedVisibleTextOffset.reset();
      LOG_DBG("ERS", "Opened for first time, navigating to text reference at index %d", textSpineIndex);
    }
  }

  loadCachedBookmarks();
  return true;
}

void EpubReaderActivity::openReaderMenu() {
  pendingManualTurn = 0;
  const int currentPage = section ? section->currentPage + 1 : 0;
  const int totalPages = section ? section->estimatedTotalPages() : 0;
  float bookProgress = 0.0f;
  if (epub->getBookSize() > 0 && section && section->estimatedTotalPages() > 0) {
    const float chapterProgress =
        static_cast<float>(section->currentPage) / static_cast<float>(section->estimatedTotalPages());
    bookProgress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
  }
  const int bookProgressPercent = clampPercent(static_cast<int>(bookProgress + 0.5f));
  startActivityForResult(std::make_unique<EpubReaderMenuActivity>(
                             renderer, mappedInput, epub->getTitle(), currentPage, totalPages, bookProgressPercent,
                             SETTINGS.orientation, !currentPageFootnotes.empty(), !cachedBookmarks.empty()),
                         [this](const ActivityResult& result) {
                           const auto& menu = std::get<MenuResult>(result.data);
                           if (SETTINGS.orientation != menu.orientation) {
                             applyOrientation(menu.orientation);
                           }
                           toggleAutoPageTurn(menu.pageTurnOption);
                           if (!result.isCancelled) {
                             onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(menu.action));
                           }
                         });
}

bool EpubReaderActivity::buildTickHeapGate() {
  const size_t freeHeap = ESP.getFreeHeap();
  const size_t maxBlock = ESP.getMaxAllocHeap();
  buildHeapPaused = freeHeap < BACKGROUND_BUILD_MIN_FREE_HEAP || maxBlock < BACKGROUND_BUILD_MIN_MAX_ALLOC;
  return !buildHeapPaused;
}

void EpubReaderActivity::showBuildPopup(GfxRenderer& renderer, int& pagesUntilFullRefresh) {
  if (!buildPopupPending || !renderer.hasFrameBuffer()) return;
  GUI.drawPopup(renderer, tr(STR_INDEXING));
  pagesUntilFullRefresh = 1;
  buildPopupPending = false;
}

void EpubReaderActivity::openDictionaryWordSelect() {
  if (SETTINGS.dictionaryName[0] == '\0') {
    showDictionaryMessage = true;
    dictionaryMessageTime = millis();
    requestUpdate();
    return;
  }
  if (!section) return;
  auto page = section->loadPage(section->currentPage);
  if (!page) return;

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;

  startActivityForResult(std::make_unique<DictionaryWordSelectActivity>(renderer, mappedInput, std::move(page),
                                                                        orientedMarginLeft, orientedMarginTop),
                         [this](const ActivityResult&) { requestUpdate(); });
}

#ifdef CROSSPOINT_BG_BUILD_TASK
void EpubReaderActivity::bgBuildTaskTrampoline(void* param) {
  static_cast<EpubReaderActivity*>(param)->bgBuildTaskLoop();
}

void EpubReaderActivity::startBgBuildTask() {
  if (bgBuildTaskHandle) return;
  bgBuildStop.store(false, std::memory_order_relaxed);
  bgBuildExited.store(false, std::memory_order_relaxed);
  bgBuildCompleteNotify.store(false, std::memory_order_relaxed);
  bgBuildFailedNotify.store(false, std::memory_order_relaxed);
#ifdef CROSSPOINT_BG_IMAGE_DECODE
  // The pre-decode interlock is process-global, not per-task, and a previous
  // stop can leave it dirty: a cancel that timed out returns with bgDecodeActive
  // still true (the decode it gave up on ends by clearing it, but nothing
  // guarantees that happened before the join), and any path that raised the
  // abort flag lowers it in its own scope. Clear both here so a restarted task
  // does not inherit "a decode is running" (every render would then pay a
  // cancel-and-wait) or "abort" (the first pre-decode would bail instantly).
  ImageBlock::endBackgroundDecode();
  ImageToFramebufferDecoder::requestAbort(false);
#endif
  // Core 0: WiFi's home core, idle while reading (loopTask and the render task
  // are both pinned to core 1). Priority 1 matches them; the RenderLock is the
  // ordering authority regardless. Stack: the parse/layout path historically
  // ran on the 8 KB loop task; 12 KB gives margin for deeper CSS/parser frames.
  constexpr uint32_t kStack = 12288;
  if (xTaskCreatePinnedToCore(&bgBuildTaskTrampoline, "ErsBgBuild", kStack, this, 1, &bgBuildTaskHandle, 0) != pdPASS) {
    bgBuildTaskHandle = nullptr;
    LOG_ERR("ERS", "Failed to create background build task; falling back to loop ticks");
  }
}

void EpubReaderActivity::stopBgBuildTask() {
  if (!bgBuildTaskHandle) return;
  // Wake the task out of its (possibly long) idle wait BEFORE raising the stop
  // flag: at give time the task cannot yet have observed stop=true, so its TCB
  // is provably alive (give-after-store leaves a theoretical window where the
  // task polls the flag, self-deletes, and the give hits a freed TCB). Worst
  // case the woken pass misses the flag and re-sleeps on the short cadence
  // (this caller holds the RenderLock, so its TryAcquire fails -> ~25 ms),
  // keeping the join bounded. It cannot start new work either way: every step
  // is picked under a TryAcquire this caller's lock defeats.
  xTaskNotifyGive(bgBuildTaskHandle);
  bgBuildStop.store(true, std::memory_order_release);
#ifdef CROSSPOINT_BG_IMAGE_DECODE
  // A pre-decode in flight would otherwise hold the join for its whole
  // remaining runtime (seconds), so cancel it -- but only AFTER the stop flag
  // is published. The cancel can time out (its 5 s cap covers the un-abortable
  // ZIP extract), and it lowers the abort flag on the way out; with the store
  // after the cancel, a task sitting between the extract and the decode saw
  // neither flag raised and went on to run a full multi-second decode inside
  // the join. Reusing the same cancel-and-wait the render path uses also means
  // the abort flag is raised and lowered inside one RenderLock scope, which is
  // what keeps that global flag from ever touching a render-task decode.
  ImageBlock::cancelBackgroundDecode();
#endif
  while (!bgBuildExited.load(std::memory_order_acquire)) {
    delay(1);
  }
  bgBuildTaskHandle = nullptr;
}

void EpubReaderActivity::bgBuildTaskLoop() {
  while (!bgBuildStop.load(std::memory_order_acquire)) {
    bool didWork = false;
    // True when this pass could not rule out imminent work (lock contended, or a
    // build is live but heap-gated this tick) — retry on the short cadence then.
    bool workPlausible = false;
    // Active-section build tick: same per-tick RenderLock scope, heap gate, and
    // page count as the loop pump this replaces — never holds the lock across a
    // chapter, so pending renders interleave exactly as before. TryAcquire, not
    // the blocking ctor: parking here would deadlock an exit path that joins
    // this task while holding the RenderLock (see RenderLock::TryAcquire). All
    // `section` access happens strictly under the lock: the loop-task idiom of
    // an unlocked pre-check is a benign stale read there, but this task runs
    // truly parallel on core 0, where an unlocked deref races ~Section.
    {
      RenderLock lock{RenderLock::TryAcquire{}};
      if (!lock.locked()) {
        workPlausible = true;
      } else if (section && section->isBuilding()) {
        workPlausible = true;
        if (buildTickHeapGate()) {
          if (!section->buildSomeMore(BACKGROUND_BUILD_PAGES_PER_TICK)) {
            // Reset/redraw belong to the loop task; just report the failure.
            bgBuildFailedNotify.store(true, std::memory_order_release);
          } else {
            didWork = true;
            if (section->isBuildComplete()) {
              bgBuildCompleteNotify.store(true, std::memory_order_release);
            }
          }
        }
      }
    }
#ifdef CROSSPOINT_NEXT_SECTION_PREBUILD
    if (!didWork && !bgBuildStop.load(std::memory_order_acquire)) {
      didWork = prebuildStep();
    }
#endif
#ifdef CROSSPOINT_BG_IMAGE_DECODE
    // Lowest-priority idle work: it is the only step here that runs for seconds
    // with the lock released, so it goes after everything the reader is
    // actually waiting on.
    if (!didWork && !bgBuildStop.load(std::memory_order_acquire)) {
      didWork = imageDecodeStep(workPlausible);
    }
#endif
    if (didWork) {
      // Back-to-back ticks while there is work; yield one tick so the render
      // task can take the lock.
      vTaskDelay(1);
    } else {
      // Idle: block on a notification instead of polling, so tickless light
      // sleep isn't held off by this task all session. renderBook() notifies
      // after resolving a new render spec (covers build starts, page turns, and
      // settings changes) and stopBgBuildTask() notifies for exit, so the long
      // timeout is only a fallback; the short one covers transient lock/heap
      // declines. Index 0 is this task's own slot — nothing else posts to it.
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(workPlausible ? 25 : 250));
    }
  }
  bgBuildExited.store(true, std::memory_order_release);
  vTaskDelete(nullptr);
}
#endif  // CROSSPOINT_BG_BUILD_TASK

#ifdef CROSSPOINT_NEXT_SECTION_PREBUILD
bool EpubReaderActivity::renderSpecEquals(const ReaderRenderSpec& a, const ReaderRenderSpec& b) {
  return a.fontId == b.fontId && a.lineCompression == b.lineCompression &&
         a.extraParagraphSpacing == b.extraParagraphSpacing && a.paragraphAlignment == b.paragraphAlignment &&
         a.viewportWidth == b.viewportWidth && a.viewportHeight == b.viewportHeight &&
         a.hyphenationEnabled == b.hyphenationEnabled && a.embeddedStyle == b.embeddedStyle &&
         a.imageRendering == b.imageRendering && a.focusReadingEnabled == b.focusReadingEnabled;
}

// One prebuild step, run on the background build task when the active section
// has nothing to build. CACHED-ONLY by design: the prebuild never calls
// startBuild(), because a second live build context would share the Epub's
// single CssParser and the html/.bin.part file paths with any build
// renderBook() starts synchronously — adversarial review found both to be racy.
// A cache-miss next spine simply falls back to today's boundary behavior. All
// work — including Section construction and loadSectionFile, which read the
// shared book.bin metadata handle — happens under the (try-acquired)
// RenderLock; a cached load is tens of ms, the same order as a build tick.
// Returns true if it made progress.
bool EpubReaderActivity::prebuildStep() {
  RenderLock lock{RenderLock::TryAcquire{}};
  if (!lock.locked()) return false;
  // Drop a stale prebuild (reader jumped/paged back, or settings changed). The
  // parked Section never has a build context, so its destructor touches no
  // shared build state (no CssParser, no .part commit).
  if (prebuiltSection && (prebuiltSpineIndex != currentSpineIndex + 1 || !lastRenderSpecValid ||
                          !renderSpecEquals(prebuiltSpec, lastRenderSpec))) {
    prebuiltSection.reset();
    prebuiltSpineIndex = -1;
    prebuildDeclinedSpine = -1;
  }
  if (prebuiltSection) return false;  // parked and ready
  // Consider prebuilding: current chapter fully built, reader near its end.
  if (!section || section->isBuilding() || section->isPartial() || !lastRenderSpecValid) return false;
  if (section->pageCount == 0 || section->currentPage + PREBUILD_NEAR_END_PAGES < static_cast<int>(section->pageCount))
    return false;
  if (currentSpineIndex + 1 >= epub->getSpineItemsCount()) return false;
  const int buildSpine = currentSpineIndex + 1;
  if (prebuildDeclinedSpine == buildSpine) return false;  // known cache miss; don't re-probe every idle tick

  auto candidate = std::unique_ptr<Section>(new Section(epub, buildSpine, renderer));
  if (!candidate->loadSectionFile(lastRenderSpec)) {
    // No usable cache for the next spine. Remember the miss so idle iterations
    // don't re-open SD files on every wake; cleared when the reader moves on.
    prebuildDeclinedSpine = buildSpine;
    return false;
  }
  prebuiltSpineIndex = buildSpine;
  prebuiltSpec = lastRenderSpec;
  prebuiltSection = std::move(candidate);
  LOG_DBG("ERS", "Prebuilt next section %d (%s)", buildSpine, prebuiltSection->isPartial() ? "partial" : "ready");
  return true;
}
#endif  // CROSSPOINT_NEXT_SECTION_PREBUILD

#ifdef CROSSPOINT_BG_IMAGE_DECODE
// One image pre-decode step, run on the background build task when there is
// nothing left to build or prebuild. First view of an in-book image costs a
// 0.5-3 s decode today, taken inside the render behind a placeholder pass; this
// moves it off the page-turn critical path by decoding the images of upcoming
// pages into their .pxc files ahead of time.
//
// Two phases, and the split is the whole design:
//
// UNDER the (try-acquired) RenderLock -- everything that touches reader state:
// pick the nearest lookahead page not already ruled out, deserialize it,
// find the first image with no usable cache, and capture its path, source
// href and geometry AS VALUES. Also captured: a shared_ptr copy of the Epub,
// the decoder pointer (resolving it touches ImageDecoderFactory's non-
// thread-safe lazy init, which only the lock serializes), and the "a background
// decode is running" flag plus the image it is running on (published here,
// before the lock is released, so no render can start without seeing them).
//
// WITHOUT the lock: extract the image out of the book if needed, then decode it
// in cacheOnly mode. Both steps touch only the SD card through Storage (which
// is mutex'd per operation), the pixel cache, and the heap -- no Section, no
// Epub member, no renderer.
//
// Why a shared_ptr copy of the Epub rather than ImageBlock's extractor hook:
// that hook's context is a raw Epub*, and launchKOReaderSync() drops the Epub
// under the RenderLock WITHOUT joining this task, to free RAM for the TLS
// handshake. (onExit's clear is safe -- it happens after stopBgBuildTask()
// joins -- but the sync path's is not.) A shared_ptr copy keeps the object
// alive for the length of this step no matter which path runs.
//
// Staleness is a non-issue: a .pxc is keyed by the image, not by the page it
// appears on, so re-pagination cannot invalidate one. The only correctness
// requirement is that two decoders never write the same file, which the
// ImageBlock interlock handles (see the note there).
bool EpubReaderActivity::imageDecodeStep(bool& workPlausible) {
  std::shared_ptr<Epub> epubRef;
  // Resolved under the lock and carried into the unlocked phase: the factory's
  // lazy singleton init is not thread-safe, so it must not be reached from
  // there (see ImageBlock::backgroundDecoderFor).
  ImageToFramebufferDecoder* decoder = nullptr;
  std::string imagePath;
  std::string srcPath;
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  int screenWidth = 0;
  int screenHeight = 0;
  int offset = 0;

  {
    RenderLock lock{RenderLock::TryAcquire{}};
    if (!lock.locked()) {
      workPlausible = true;
      return false;
    }
    // A building section re-numbers pages under the scan, and the margins
    // snapshot only exists once a render has happened.
    if (!epub || !section || section->isBuilding() || !lastRenderMarginsValid) return false;

    // The cursor belongs to one reading position; the reader moving re-arms the
    // whole window. Only this task touches these three.
    if (imageDecodeSpine != currentSpineIndex || imageDecodeBasePage != section->currentPage) {
      imageDecodeSpine = currentSpineIndex;
      imageDecodeBasePage = section->currentPage;
      imageDecodeDeclined = 0;
    }
    if (ESP.getFreeHeap() < IMAGE_DECODE_MIN_FREE_HEAP || ESP.getMaxAllocHeap() < IMAGE_DECODE_MIN_MAX_ALLOC)
      return false;

    std::unique_ptr<Page> page;
    // The page this scan reads: either `page` above, or the resident cache entry
    // borrowed in place (see below). Valid only inside this locked scope.
    const Page* scanPage = nullptr;
    for (int i = 1; i <= IMAGE_DECODE_LOOKAHEAD; i++) {
      if (imageDecodeDeclined & (1u << i)) continue;
      const int pageNumber = section->currentPage + i;
      if (pageNumber >= static_cast<int>(section->pageCount)) {
        imageDecodeDeclined |= (1u << i);  // past the end of the chapter
        continue;
      }
      offset = i;
#if defined(CROSSPOINT_PAGE_CACHE) && defined(CROSSPOINT_BG_IMAGE_DECODE)
      // Composition hook for PR #3050 (CROSSPOINT_PAGE_CACHE), whose one-entry
      // page cache usually holds exactly this page (currentPage + 1 is the first
      // lookahead slot); deserializing a second copy of it would cost the SD read
      // that cache exists to avoid, plus a second live Page. Borrow it instead:
      // the scan below is read-only (const element access), so nothing here can
      // disturb the entry the next forward turn is going to consume. Full key
      // check, same fields and same lock as takeCachedPage -- minus the consume:
      // the entry is left in place, and any mismatch just falls through to the
      // deserialize. Undefined on this branch; compiled in whenever both flags
      // are on, in either merge order.
      if (cachedPage && cachedPageGeneration == sectionGeneration && cachedPageSpine == currentSpineIndex &&
          cachedPageNumber == pageNumber && cachedPagePageCount == section->pageCount &&
          cachedPagePartial == section->isPartial()) {
        scanPage = cachedPage.get();
        break;
      }
#endif
      page = section->loadPage(pageNumber);
      scanPage = page.get();
      break;
    }
    if (offset == 0) return false;  // window exhausted until the reader moves
    if (!scanPage) {
      imageDecodeDeclined |= (1u << offset);
      return false;
    }

    screenWidth = renderer.getScreenWidth();
    screenHeight = renderer.getScreenHeight();
    for (const auto& element : scanPage->elements) {
      if (element->getTag() != TAG_PageImage) continue;
      const auto& pageImage = static_cast<const PageImage&>(*element);
      const ImageBlock& block = pageImage.getImageBlock();
      if (!block.needsDecode()) continue;
      // Exactly the geometry ImageBlock::render() would use, including its
      // bounds check: an image the render would reject is not worth decoding.
      const int px = pageImage.xPos + lastRenderMarginLeft;
      const int py = pageImage.yPos + lastRenderMarginTop;
      if (px < 0 || py < 0 || px + block.getWidth() > screenWidth || py + block.getHeight() > screenHeight) continue;
      decoder = ImageBlock::backgroundDecoderFor(block.getImagePath());
      if (!decoder) continue;  // no decoder for this format
      imagePath = block.getImagePath();
      srcPath = block.getSourcePath();
      x = px;
      y = py;
      width = block.getWidth();
      height = block.getHeight();
      break;
    }
    if (imagePath.empty()) {
      imageDecodeDeclined |= (1u << offset);  // nothing left to decode on this page
      return false;
    }

    epubRef = epub;
    ImageBlock::beginBackgroundDecode(imagePath);
  }

  // ---- No RenderLock held from here ----------------------------------------
  if (!srcPath.empty() && !Storage.exists(imagePath.c_str())) {
    // The one step of this that cannot be aborted; it is bounded by the
    // image's stored size (see BG_DECODE_CANCEL_TIMEOUT_MS).
    if (!epubRef->extractItemToFile(srcPath, imagePath)) {
      LOG_DBG("ERS", "Pre-decode extraction failed: %s", srcPath.c_str());
    }
  }

  bool decoded = false;
  // Whoever wanted this decode stopped raised the abort flag while we were in
  // the extract above, which is the one step that cannot honor it. Starting the
  // decode anyway is the bad case: the canceller may give up waiting and lower
  // the flag (BG_DECODE_CANCEL_TIMEOUT_MS) precisely because the extract took
  // that long, and the decode would then run to completion -- as a second
  // writer on the file the render task is about to produce, or for seconds
  // inside an exit join.
  bool aborted = ImageToFramebufferDecoder::abortRequested();
  if (!aborted && !bgBuildStop.load(std::memory_order_acquire)) {
    const unsigned long t0 = millis();
    decoded =
        ImageBlock::decodeToCacheOnly(renderer, decoder, imagePath, x, y, width, height, screenWidth, screenHeight);
    if (decoded) {
      LOG_DBG("ERS", "Pre-decoded %s in %lums", imagePath.c_str(), millis() - t0);
    } else {
      // Read BEFORE endBackgroundDecode(): a canceller only lowers the flag
      // once it observes the decode has stopped, so while the "active" flag is
      // still up the abort flag cannot go back down under this read.
      aborted = ImageToFramebufferDecoder::abortRequested();
    }
  }
  ImageBlock::endBackgroundDecode();

  if (aborted) {
    // Not a property of the image: the render task took it over (or the reader
    // is leaving). Declining here would disarm this lookahead slot until the
    // reader moves, and on an image-dense chapter the takeover happens on
    // exactly the pages worth pre-decoding. Leave the cursor alone and retry.
    LOG_DBG("ERS", "Pre-decode aborted: %s", imagePath.c_str());
    return false;
  }
  if (!decoded) {
    // Heap, format, or a corrupt file -- a property of this image, so stop
    // offering this page: the render path still decodes whatever it needs, and
    // retrying here every 25 ms would not help.
    imageDecodeDeclined |= (1u << offset);
    return false;
  }
  // Deliberately NOT declined on success: the next pass re-scans the same page
  // for a second image, and declines it once there is nothing left.
  return true;
}
#endif  // CROSSPOINT_BG_IMAGE_DECODE

void EpubReaderActivity::loop() {
  if (!epub) {
    finish();
    return;
  }

  constexpr unsigned long IDLE_PREWARM_DEBOUNCE_MS = 400;
  if (section && !section->isBuilding() && !RenderLock::peek() && renderer.hasFrameBuffer() &&
      lastRenderCompleteMs != 0 && millis() - lastRenderCompleteMs > IDLE_PREWARM_DEBOUNCE_MS &&
      ESP.getFreeHeap() > RENDER_MIN_FREE_HEAP && ESP.getMaxAllocHeap() > BACKGROUND_BUILD_MIN_MAX_ALLOC &&
      (idlePrewarmSpine != currentSpineIndex || idlePrewarmPage != section->currentPage)) {
    RenderLock lock;
    if (section && !section->isBuilding() &&
        (idlePrewarmSpine != currentSpineIndex || idlePrewarmPage != section->currentPage)) {
      idlePrewarmSpine = currentSpineIndex;
      idlePrewarmPage = section->currentPage;
      const int nextPage = section->currentPage + 1;
      if (nextPage < static_cast<int>(section->pageCount)) {
        if (const auto p = section->loadPage(nextPage)) {
          if (auto* fcm = renderer.getFontCacheManager()) {
            const auto t0 = millis();
            auto scope = fcm->createPrewarmScope();
            p->render(renderer, SETTINGS.getReaderFontId(), 0, 0);
            scope.endScanAndPrewarm();
            LOG_DBG("ERS", "Idle prewarm: page %d in %lums", nextPage, millis() - t0);
          }
        }
      }
    }
  }

  if (section && !section->isBuilding() && section->isPartial() && !RenderLock::peek() && buildViewportWidth > 0 &&
      !partialRebuildStartFailed &&
      section->currentPage + PARTIAL_REBUILD_START_MARGIN >= static_cast<int>(section->pageCount)) {
    RenderLock lock;
#ifdef CROSSPOINT_BG_IMAGE_DECODE
    // Same reason as the renderBook()-site cancel: this build's parse pump (the
    // buildSomeMore ticks that follow, wherever they run) extracts images out of
    // the book to probe their dimensions, and a pre-decode in flight extracts
    // too, with no lock -- both derive the same destination path from the
    // book-internal href, so the two would be writing the SAME file. Stop it
    // before the build exists. The wait is bounded by the cancel timeout and
    // this is deferrable background work, not a page turn; on a timeout the
    // overlap stays open exactly as it does at the render site.
    ImageBlock::cancelBackgroundDecode();
#endif
    const ReaderRenderSpec buildSpec = SETTINGS.readerRenderSpec(buildViewportWidth, buildViewportHeight);
    if (!section->startBuild(buildSpec)) {
      partialRebuildStartFailed = true;
      LOG_ERR("ERS", "Failed to start deferred partial extension build");
    } else {
      LOG_DBG("ERS", "Reader near partial watermark (%d/%d), resuming extension build", section->currentPage,
              section->pageCount);
#ifdef CROSSPOINT_BG_BUILD_TASK
      // The one build start that doesn't flow through a render: wake the build
      // task directly so pickup isn't left to its long idle-wait fallback.
      if (bgBuildTaskHandle) xTaskNotifyGive(bgBuildTaskHandle);
#endif
    }
  }

#ifdef CROSSPOINT_BG_BUILD_TASK
  // The core-0 build task owns the pump. Consume its completion/failure
  // notifications here in the loop task, so section reset, reposition, and
  // redraw run in their usual context; the loop-tick pump below stays only as
  // a runtime fallback for the (never observed) case that task creation failed.
  if (bgBuildFailedNotify.exchange(false, std::memory_order_acq_rel)) {
    RenderLock lock;
    LOG_ERR("ERS", "Background section build failed");
    section.reset();
    requestUpdate();
  }
  if (bgBuildCompleteNotify.exchange(false, std::memory_order_acq_rel)) {
    RenderLock lock;
    // cppcheck-suppress knownConditionTrueFalse
    if (section && section->isBuildComplete() && applyDeferredReposition()) {
      requestUpdate();
    }
  }
  if (bgBuildTaskHandle == nullptr && section && section->isBuilding() && !RenderLock::peek() &&
      (section->isPartial() || static_cast<int>(section->pageCount) < section->currentPage + BUILD_WINDOW_AHEAD) &&
      buildTickHeapGate()) {
#else
  if (section && section->isBuilding() && !RenderLock::peek() &&
      (section->isPartial() || static_cast<int>(section->pageCount) < section->currentPage + BUILD_WINDOW_AHEAD) &&
      buildTickHeapGate()) {
#endif
    RenderLock lock;
    if (section->isBuilding() && buildTickHeapGate()) {
      if (!section->buildSomeMore(BACKGROUND_BUILD_PAGES_PER_TICK)) {
        LOG_ERR("ERS", "Background section build failed");
        section.reset();
        requestUpdate();
      } else if (section->isBuildComplete() && applyDeferredReposition()) {
        requestUpdate();
      }
    }
  }

  const bool atEndOfBook = currentSpineIndex > 0 && currentSpineIndex >= epub->getSpineItemsCount();
  clearEndOfBookOptionsIfNeeded();

  if (SETTINGS.removeReadBooksFromRecents) {
    if (atEndOfBook && !recentsEntryRemoved) {
      recentsEntryRemoved = RECENT_BOOKS.removeByPath(epub->getPath());
    } else if (!atEndOfBook && recentsEntryRemoved) {
      RECENT_BOOKS.addBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
      recentsEntryRemoved = false;
    }
  }

  if (atEndOfBook) {
    pendingReadFolderMove = SETTINGS.moveFinishedToReadFolder && !isInReadFolder(epub->getPath());
  } else {
    pendingReadFolderMove = false;
  }

  const auto touch = ReaderUtils::detectTouchPageTurn(renderer, mappedInput);

  if (automaticPageTurnActive) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        ReaderUtils::isTouchMenuGesture(renderer, mappedInput)) {
      automaticPageTurnActive = false;
      requestUpdate();
      return;
    }

    if (!section) {
      requestUpdate();
      return;
    }

    if (RenderLock::peek()) {
      lastPageTurnTime = millis();
      return;
    }

    if ((millis() - lastPageTurnTime) >= pageTurnDuration) {
      pageTurn(true);
      requestUpdate();
      return;
    }
  }

  if (showBookmarkMessage && (millis() - bookmarkMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showBookmarkMessage = false;
    requestUpdate();
  }

  if (showDictionaryMessage && (millis() - dictionaryMessageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showDictionaryMessage = false;
    requestUpdate();
  }

  const bool confirmReleased = mappedInput.wasReleased(MappedInputManager::Button::Confirm);
  if (confirmReleased) {
    switch (SETTINGS.longPressMenuFunction) {
      case CrossPointSettings::LP_MENU_BOOKMARK:
        if (mappedInput.getHeldTime() >= ReaderUtils::BOOKMARK_HOLD_MS) {
          addBookmark();
          showBookmarkMessage = true;
          bookmarkMessageTime = millis();
          requestUpdate();
          return;
        }
        break;
      case CrossPointSettings::LP_MENU_KOSYNC:
        if (mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS && launchKOReaderSync()) return;
        break;
      case CrossPointSettings::LP_MENU_DICTIONARY:
        if (mappedInput.getHeldTime() >= ReaderUtils::BOOKMARK_HOLD_MS) {
          openDictionaryWordSelect();
          return;
        }
        break;
      case CrossPointSettings::LP_MENU_READER_MENU:
        // Confirm already opens the menu on release. This option exists for
        // boards whose capacitive Home key supplies the long-press action.
        break;
      case CrossPointSettings::LP_MENU_DISABLED:
      default:
        break;
    }
  }

  // Home-key boards have no front Confirm button, so a Home-key hold runs the
  // same user-selected long-press action. The SDK emits this event once per
  // hold and suppresses the short Home tap for the same contact.
  if (mappedInput.wasHomeKeyHold()) {
    switch (SETTINGS.longPressMenuFunction) {
      case CrossPointSettings::LP_MENU_BOOKMARK:
        if (!showBookmarkMessage) {
          addBookmark();
          showBookmarkMessage = true;
          bookmarkMessageTime = millis();
          requestUpdate();
        }
        return;
      case CrossPointSettings::LP_MENU_KOSYNC:
        launchKOReaderSync();
        return;
      case CrossPointSettings::LP_MENU_DICTIONARY:
        if (!showDictionaryMessage) {
          openDictionaryWordSelect();
        }
        return;
      case CrossPointSettings::LP_MENU_READER_MENU:
        openReaderMenu();
        return;
      case CrossPointSettings::LP_MENU_DISABLED:
      default:
        break;
    }
  }

  if (handleEndOfBookMenu()) {
    return;
  }

  if (confirmReleased || ReaderUtils::isTouchMenuGesture(renderer, mappedInput)) {
    openReaderMenu();
  }

  if (footnoteDepth > 0 && mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_BACK_OR_HOME_MS) {
    restoreSavedPosition();
    return;
  }

  if (handleBackNavigation()) {
    return;
  }

  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FOOTNOTES &&
      mappedInput.wasReleased(MappedInputManager::Button::Power) &&
      !mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (footnoteDepth > 0) {
      restoreSavedPosition();
    } else {
      if (currentPageFootnotes.size() == 1) {
        navigateToHref(currentPageFootnotes[0].href, true);
      } else if (currentPageFootnotes.size() > 1) {
        startActivityForResult(
            std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
            [this](const ActivityResult& result) {
              if (!result.isCancelled) {
                const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                navigateToHref(footnoteResult.href, true);
              }
              requestUpdate();
            });
      }
    }
    return;
  }

  constexpr unsigned long kMinManualTurnGapMs = 200;
  const bool turnGuardActive = RenderLock::peek() || (millis() - lastPageTurnTime) < kMinManualTurnGapMs;
  if (pendingManualTurn != 0 && !turnGuardActive) {
    if (!section) {
      pendingManualTurn = 0;
      return;
    }
    const bool forward = pendingManualTurn > 0;
    pendingManualTurn = 0;
    pageTurn(forward);
    requestUpdate();
    return;
  }

  auto [prevTriggered, nextTriggered, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  prevTriggered = prevTriggered || touch.prev;
  nextTriggered = nextTriggered || touch.next;
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  if (handleEndOfBookPageTurn(prevTriggered, nextTriggered)) {
    return;
  }

  const unsigned long heldMs = (touch.prev || touch.next) ? touch.heldMs : mappedInput.getHeldTime();
  const bool longPress = !fromTilt && heldMs > ReaderUtils::SKIP_HOLD_MS;

  if (mappedInput.wasReleased(MappedInputManager::Button::Power) &&
      mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    return;
  }

  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.CHAPTER_SKIP) {
    skipPages(nextTriggered ? 1 : -1);
    requestUpdate();
    return;
  }

  if (longPress && SETTINGS.longPressButtonBehavior == SETTINGS.ORIENTATION_CHANGE) {
    const uint8_t newOrientation =
        nextTriggered ? (SETTINGS.orientation - 1 + SETTINGS.ORIENTATION_COUNT) % SETTINGS.ORIENTATION_COUNT
                      : (SETTINGS.orientation + 1) % SETTINGS.ORIENTATION_COUNT;
    applyOrientation(newOrientation);
    requestUpdate();
    return;
  }

  if (!section) {
    requestUpdate();
    return;
  }

  if (turnGuardActive) {
    pendingManualTurn = prevTriggered ? -1 : 1;
    return;
  }

  if (prevTriggered) {
    pageTurn(false);
  } else {
    pageTurn(true);
  }
  requestUpdate();
}

void EpubReaderActivity::jumpToPercent(int percent) {
  if (!epub) return;
  const size_t bookSize = epub->getBookSize();
  if (bookSize == 0) return;

  percent = clampPercent(percent);

  size_t targetSize =
      (bookSize / 100) * static_cast<size_t>(percent) + (bookSize % 100) * static_cast<size_t>(percent) / 100;
  if (percent >= 100) targetSize = bookSize - 1;

  const int spineCount = epub->getSpineItemsCount();
  if (spineCount == 0) return;

  int targetSpineIndex = spineCount - 1;
  size_t prevCumulative = 0;

  for (int i = 0; i < spineCount; i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      targetSpineIndex = i;
      prevCumulative = (i > 0) ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = (cumulative > prevCumulative) ? (cumulative - prevCumulative) : 0;
  pendingSpineProgress =
      (spineSize == 0) ? 0.0f : static_cast<float>(targetSize - prevCumulative) / static_cast<float>(spineSize);
  pendingSpineProgress = std::clamp(pendingSpineProgress, 0.0f, 1.0f);

  {
    RenderLock lock;
    clearDeferredReposition();
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
    pendingPercentJump = true;
    section.reset();
  }
  requestUpdate();
}

void EpubReaderActivity::onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action) {
  auto progressChangeResultHandler = [this](const ActivityResult& result) {
    loadCachedBookmarks();
    if (result.isCancelled) {
      openReaderMenu();
    } else {
      const auto& sync = std::get<ProgressChangeResult>(result.data);

      if (sync.hasVisibleTextOffset && sync.spineIndex >= 0 && sync.spineIndex < epub->getSpineItemsCount()) {
        RenderLock lock;
        clearDeferredReposition();
        if (section && currentSpineIndex == sync.spineIndex) {
          const auto page = section->getPageForVisibleTextOffset(sync.visibleTextOffset);
          section->currentPage = page.value_or(std::max(0, sync.page));
        } else {
          currentSpineIndex = sync.spineIndex;
          pendingOffsetJump = sync.visibleTextOffset;
          nextPageNumber = std::max(0, sync.page);
          section.reset();
        }
        requestUpdate();
        return;
      }

      int targetSpineIndex = sync.spineIndex;
      int targetPage = sync.page;
      const int activeTotalPages = section ? section->estimatedTotalPages() : 0;
      const bool cachedPageMatchesActiveSection = section && sync.totalPages > 0 &&
                                                  currentSpineIndex == sync.spineIndex && sync.page >= 0 &&
                                                  sync.page < sync.totalPages && activeTotalPages == sync.totalPages;

      if (!cachedPageMatchesActiveSection && sync.hasSavedProgress) {
        const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
        CrossPointPosition fallback =
            ProgressMapper::toCrossPoint(epub, {sync.xpath, sync.percentage}, renderer, currentSpineIndex, totalPages);
        targetSpineIndex = fallback.spineIndex;
        targetPage = fallback.pageNumber;
      }

      RenderLock lock;
      clearDeferredReposition();

      if (currentSpineIndex != targetSpineIndex) {
        currentSpineIndex = targetSpineIndex;
        nextPageNumber = targetPage;
        section.reset();
      } else if (section && section->currentPage != targetPage) {
        const int clampedTargetPage = std::max(0, targetPage);
        section->currentPage = clampedTargetPage;
      } else if (!section) {
        nextPageNumber = targetPage;
      }
      requestUpdate();
    }
  };

  switch (action) {
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      const int spineIdx = currentSpineIndex;
      startActivityForResult(
          std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub, spineIdx),
          [this](const ActivityResult& result) {
            if (result.isCancelled) {
              openReaderMenu();
              return;
            }
            const auto& chapterResult = std::get<ChapterResult>(result.data);
            RenderLock lock;
            clearDeferredReposition();
            currentSpineIndex = chapterResult.spineIndex;
            pendingAnchor = chapterResult.anchor;
            nextPageNumber = 0;
            section.reset();
            requestUpdate();
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::FOOTNOTES: {
      startActivityForResult(std::make_unique<EpubReaderFootnotesActivity>(renderer, mappedInput, currentPageFootnotes),
                             [this](const ActivityResult& result) {
                               if (result.isCancelled) {
                                 openReaderMenu();
                                 return;
                               }
                               const auto& footnoteResult = std::get<FootnoteResult>(result.data);
                               navigateToHref(footnoteResult.href, true);
                               requestUpdate();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TEXT_SETTINGS: {
      startActivityForResult(std::make_unique<TextSettingsActivity>(renderer, mappedInput, &sdFontSystem.registry(),
                                                                    TextSettingsActivity::Tab::Family),
                             [this](const ActivityResult&) {
                               {
                                 RenderLock lock;
                                 if (section) {
                                   rememberCurrentContentOffset();
                                   cachedSpineIndex = currentSpineIndex;
                                   cachedChapterTotalPageCount = section->pageCount;
                                   nextPageNumber = section->currentPage;
                                 }
                                 section.reset();
                               }
                               openReaderMenu();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::NIGHT_MODE:
      // Handled in-place by EpubReaderMenuActivity so its On/Off value updates
      // without closing the menu.
      break;
    case EpubReaderMenuActivity::MenuAction::FRONTLIGHT:
      // Handled in-place by EpubReaderMenuActivity using the live frontlight HAL.
      break;
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
            if (result.isCancelled) {
              openReaderMenu();
            } else {
              jumpToPercent(std::get<PercentResult>(result.data).percent);
            }
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DICTIONARY: {
      openDictionaryWordSelect();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DISPLAY_QR: {
      if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
        std::string fullText = section->getTextFromSectionFile();
        if (!fullText.empty()) {
          startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, fullText),
                                 [this](const ActivityResult&) { openReaderMenu(); });
          break;
        }
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_HOME: {
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE: {
      {
        RenderLock lock;
        if (epub && section) {
          uint16_t backupSpine = currentSpineIndex;
          uint16_t backupPage = section->currentPage;
          uint16_t backupPageCount = section->pageCount;
          section.reset();
          epub->clearCache();
          epub->setupCacheDir();
          if (!saveProgress(backupSpine, backupPage, backupPageCount)) {
            LOG_ERR("ERS", "Failed to save progress before cache clear");
          }
        }
      }
      onGoHome();
      return;
    }
    case EpubReaderMenuActivity::MenuAction::SCREENSHOT: {
      {
        RenderLock lock;
        pendingScreenshot = true;
      }
      requestUpdate();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::SYNC: {
      launchKOReaderSync();
      break;
    }
    case EpubReaderMenuActivity::MenuAction::BOOKMARKS: {
      startActivityForResult(
          std::make_unique<EpubReaderBookmarksActivity>(renderer, mappedInput, epub, epub->getPath()),
          progressChangeResultHandler);
      break;
    }
    case EpubReaderMenuActivity::MenuAction::TOGGLE_BOOKMARK: {
      addBookmark();
      break;
    }
  }
}

bool EpubReaderActivity::launchKOReaderSync() {
  if (!KOREADER_STORE.hasCredentials()) return false;

  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
  std::optional<uint16_t> paragraphIndex;
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    const uint16_t paragraphPage =
        currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
    if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
      paragraphIndex = *pIdx;
    }
  }

  CrossPointPosition localPos = getCurrentPosition();
  SavedProgressPosition localKoPos = ProgressMapper::toSavedProgress(epub, localPos);
  const int tocIdx = epub->getTocIndexForSpineIndex(currentSpineIndex);
  std::string localChapterName = (tocIdx >= 0) ? epub->getTocItem(tocIdx).title : "";
  const std::string savedEpubPath = epub->getPath();

  if (!saveProgress(currentSpineIndex, currentPage, totalPages)) {
    LOG_ERR("KOSync", "Aborting sync because current progress could not be saved");
    pendingSyncSaveError = true;
    requestUpdate();
    return true;
  }

  LOG_DBG("KOSync", "Releasing epub for sync (heap before: %u)", (unsigned)ESP.getFreeHeap());
  {
    RenderLock lock;
    if (section) {
      nextPageNumber = section->currentPage;
    }
    ImageBlock::setExtractor(nullptr, nullptr);
#ifdef CROSSPOINT_NEXT_SECTION_PREBUILD
    // Freeing RAM for the handshake is the entire point of this block, and a
    // parked prebuild is both a Section of its own and a shared_ptr keeping the
    // Epub alive past the reset below. Dropping it here also keeps the "heap
    // after" log honest.
    prebuiltSection.reset();
    prebuiltSpineIndex = -1;
    prebuildDeclinedSpine = -1;
#endif
    section.reset();
    epub.reset();
  }
  LOG_DBG("KOSync", "Epub released (heap after: %u)", (unsigned)ESP.getFreeHeap());

  activityManager.replaceActivity(std::make_unique<KOReaderSyncActivity>(
      renderer, mappedInput, savedEpubPath, currentSpineIndex, currentPage, totalPages, std::move(localKoPos),
      std::move(localChapterName), paragraphIndex));
  return true;
}

void EpubReaderActivity::applyOrientation(const uint8_t orientation) {
  if (SETTINGS.orientation == orientation) {
    return;
  }

  RenderLock lock(*this);
  if (section) {
    rememberCurrentContentOffset();
    cachedSpineIndex = currentSpineIndex;
    cachedChapterTotalPageCount = section->pageCount;
    nextPageNumber = section->currentPage;
  }

  SETTINGS.orientation = orientation;
  SETTINGS.saveToFile();
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  section.reset();
}

void EpubReaderActivity::toggleAutoPageTurn(const uint8_t selectedPageTurnOption) {
  if (selectedPageTurnOption == 0 || selectedPageTurnOption >= std::size(PAGE_TURN_RATES)) {
    automaticPageTurnActive = false;
    return;
  }

  lastPageTurnTime = millis();
  pageTurnDuration = (1UL * 60 * 1000) / PAGE_TURN_RATES[selectedPageTurnOption];
  automaticPageTurnActive = true;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
    RenderLock lock;
    if (section) {
      rememberCurrentContentOffset();
      cachedSpineIndex = currentSpineIndex;
      cachedChapterTotalPageCount = section->pageCount;
      nextPageNumber = section->currentPage;
    }
    section.reset();
  }
}

bool EpubReaderActivity::pageTurn(bool isForwardTurn) {
  if (!section) return false;
  {
    RenderLock lock;
    clearDeferredReposition();
  }
  if (isForwardTurn) {
    if (section->currentPage < section->pageCount - 1 || section->isBuilding()) {
      section->currentPage++;
      lastPageTurnTime = millis();
      return true;
    } else if (currentSpineIndex + 1 < epub->getSpineItemsCount()) {
      RenderLock lock;
      nextPageNumber = 0;
      currentSpineIndex++;
#ifdef CROSSPOINT_NEXT_SECTION_PREBUILD
      if (prebuiltSection && prebuiltSpineIndex == currentSpineIndex && lastRenderSpecValid &&
          renderSpecEquals(prebuiltSpec, lastRenderSpec)) {
        // Swap the prebuilt Section in: renderBook() sees a loaded section and
        // skips the synchronous load-or-build entirely. Mirror the cache-hit
        // side effects of renderBook()'s construction path; a still-building
        // prebuild continues via the existing incremental machinery.
        section = std::move(prebuiltSection);
        prebuiltSpineIndex = -1;
        prebuildDeclinedSpine = -1;
        section->currentPage = 0;
        cachedChapterTotalPageCount = 0;
        cachedVisibleTextOffset.reset();
        partialRebuildStartFailed = false;
#ifdef CROSSPOINT_PAGE_CACHE
        // Composition hook for PR #3050 (CROSSPOINT_PAGE_CACHE), which keys its
        // one-entry page cache on sectionGeneration and documents renderBook()
        // as the only site that installs a `section`. This adoption is the
        // second such site, and the entry it would leave behind names the
        // previous chapter's pagination. Undefined on this branch; compiled in
        // whenever both flags are on, in either merge order.
        sectionGeneration++;
        dropCachedPage();
#endif
      } else {
        prebuiltSection.reset();
        prebuiltSpineIndex = -1;
        prebuildDeclinedSpine = -1;
        section.reset();
      }
#else
      section.reset();
#endif
      lastPageTurnTime = millis();
      return true;
    } else {
      currentSpineIndex = epub->getSpineItemsCount();
      lastPageTurnTime = millis();
      return true;
    }
  } else {
    if (section->currentPage > 0) {
      section->currentPage--;
      lastPageTurnTime = millis();
      return true;
    } else if (currentSpineIndex > 0) {
      RenderLock lock;
      nextPageNumber = 0;
      pendingPageJump = std::numeric_limits<uint16_t>::max();
      currentSpineIndex--;
      section.reset();
      lastPageTurnTime = millis();
      return true;
    }
  }
  return false;
}

bool EpubReaderActivity::skipPages(int amount) {
  if (!section) return false;
  if (amount > 0) {
    RenderLock lock;
    nextPageNumber = 0;
    currentSpineIndex++;
    section.reset();
    return true;
  } else {
    if (section->currentPage > 0) {
      section->currentPage = 0;
      return true;
    } else if (currentSpineIndex > 0) {
      RenderLock lock;
      nextPageNumber = 0;
      currentSpineIndex--;
      section.reset();
      return true;
    }
  }
  return false;
}

bool EpubReaderActivity::isAtEndOfBook() const { return epub && currentSpineIndex >= epub->getSpineItemsCount(); }

void EpubReaderActivity::onReturnFromEndOfBook() {
  if (epub && epub->getSpineItemsCount() > 0) {
    currentSpineIndex = epub->getSpineItemsCount() - 1;
    nextPageNumber = 0;
    pendingPageJump = std::numeric_limits<uint16_t>::max();
  }
}

bool EpubReaderActivity::skipLoopDelay() {
#ifdef CROSSPOINT_BG_BUILD_TASK
  // With the core-0 build task, loop() no longer needs fast full-clock ticks to
  // pump the build (the battery cost the loop-driven design paid); only fall
  // back to loop pacing if the task failed to start. Builds may then run at the
  // idle CPU clock — slower per page but still seconds per chapter, off-core.
  // The short-circuit also keeps buildHeapPaused a single-writer field: with the
  // task alive, only the task calls buildTickHeapGate().
  if (bgBuildTaskHandle != nullptr) return false;
#endif
  return section && section->isBuilding() && !buildHeapPaused &&
         (section->isPartial() || static_cast<int>(section->pageCount) < section->currentPage + BUILD_WINDOW_AHEAD);
}

void EpubReaderActivity::renderBook() {
  if (!epub) return;

  const auto showPendingSyncSaveError = [this]() {
    if (!pendingSyncSaveError) return;
    pendingSyncSaveError = false;
    GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
  };

  const auto showBuildError = [this]() {
    renderer.clearScreen();
    GUI.drawPopup(renderer, tr(STR_INDEX_FAILED));
    automaticPageTurnActive = false;
  };

  if (currentSpineIndex < 0) currentSpineIndex = 0;
  if (currentSpineIndex > epub->getSpineItemsCount()) currentSpineIndex = epub->getSpineItemsCount();

  if (currentSpineIndex == epub->getSpineItemsCount()) {
    return;
  }

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;
  orientedMarginRight += SETTINGS.screenMargin;

  const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();

  if (automaticPageTurnActive &&
      (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight())) {
    orientedMarginBottom +=
        std::max(SETTINGS.screenMargin,
                 static_cast<uint8_t>(statusBarHeight + UITheme::getInstance().getMetrics().statusBarVerticalMargin));
  } else {
    orientedMarginBottom += std::max(SETTINGS.screenMargin, statusBarHeight);
  }

  const uint16_t viewportWidth = renderer.getScreenWidth() - orientedMarginLeft - orientedMarginRight;
  const uint16_t viewportHeight = renderer.getScreenHeight() - orientedMarginTop - orientedMarginBottom;
  buildViewportWidth = viewportWidth;
  buildViewportHeight = viewportHeight;

  const ReaderRenderSpec renderSpec = SETTINGS.readerRenderSpec(viewportWidth, viewportHeight);
#ifdef CROSSPOINT_NEXT_SECTION_PREBUILD
  // Snapshot for the prebuild task (renderBook runs under the RenderLock here;
  // the task only reads these under the same lock). Also invalidates stale
  // prebuilds: the task compares its captured spec against this every pass.
  lastRenderSpec = renderSpec;
  lastRenderSpecValid = true;
#endif
#ifdef CROSSPOINT_BG_IMAGE_DECODE
  // Snapshot for the image pre-decode (same lock, same reason as above): these
  // are the offsets renderContents hands to Page::render, so an image's on-page
  // origin is xPos/yPos plus these. The pre-decode must match them exactly --
  // the screen clip and the dither phase both key off absolute position.
  lastRenderMarginLeft = static_cast<int16_t>(orientedMarginLeft);
  lastRenderMarginTop = static_cast<int16_t>(orientedMarginTop);
  lastRenderMarginsValid = true;
#endif
#ifdef CROSSPOINT_BG_BUILD_TASK
  // Arm the build task: nearly every state change it cares about (build
  // started, page turned, spec changed) flows through a render, so one notify
  // here retires its need to idle-poll — the lazy partial-extension start in
  // loop() is the exception and notifies directly. The task wakes, fails the
  // TryAcquire while this render holds the lock, and settles on the short retry
  // cadence until the lock frees.
  if (bgBuildTaskHandle) xTaskNotifyGive(bgBuildTaskHandle);
#endif

  if (!section) {
    const auto filepath = epub->getSpineItem(currentSpineIndex).href;
    LOG_DBG("ERS", "Loading file: %s, index: %d", filepath.c_str(), currentSpineIndex);
#ifdef CROSSPOINT_BG_IMAGE_DECODE
    // Everything below -- the Section, its loadSectionFile, and the build this
    // may start -- can run the chapter parser, which extracts images out of the
    // book to probe their dimensions. A pre-decode in flight extracts too, with
    // no lock, and the two would be writing the SAME file (both derive the path
    // from the book-internal href). Stop it first; the wait is bounded by the
    // cancel timeout and this is already the slow path (a chapter load), not a
    // page turn. On a timeout the overlap stays open exactly as it is today --
    // there is no useful way for a render to decline loading its chapter -- but
    // that cap is a liveness backstop, not an expected path.
    ImageBlock::cancelBackgroundDecode();
#endif
    section = std::unique_ptr<Section>(new Section(epub, currentSpineIndex, renderer));
    partialRebuildStartFailed = false;

    const bool cacheLoaded = section->loadSectionFile(renderSpec);
    if (cacheLoaded) {
      cachedChapterTotalPageCount = 0;
      cachedVisibleTextOffset.reset();
    }
    const bool cacheComplete = cacheLoaded && !section->isPartial();
    const bool explicitOffsetJump = pendingOffsetJump.has_value();
    const std::optional<uint32_t> offsetJump =
        explicitOffsetJump ? pendingOffsetJump
        : (pendingPageJump.has_value() || !pendingAnchor.empty() || currentSpineIndex != cachedSpineIndex)
            ? std::nullopt
            : cachedVisibleTextOffset;
    if (!cacheComplete) {
      if (section->isPartial()) {
        LOG_DBG("ERS", "Partial cache found (%d pages), resuming build...", section->pageCount);
      } else {
        LOG_DBG("ERS", "Cache not found, building...");
      }

      const bool needsFullBuild = pendingPercentJump;
      if (needsFullBuild) {
        GUI.drawPopup(renderer, tr(STR_INDEXING));
        pagesUntilFullRefresh = 1;
        const auto popupFn = [this]() {
          if (renderer.hasFrameBuffer()) GUI.drawPopup(renderer, tr(STR_INDEXING));
        };
        GfxRenderer::FrameBufferLoan loan(renderer);
        if (!section->createSectionFile(renderSpec, popupFn)) {
          LOG_ERR("ERS", "Failed to persist page data to SD");
          section.reset();
          loan.end();
          showBuildError();
          return;
        }
        loan.end();
      } else {
        const int target = pendingPageJump.has_value() ? *pendingPageJump : (nextPageNumber < 0 ? 0 : nextPageNumber);
        const bool anchorJump = !pendingAnchor.empty();

        if (section->isPartial() &&
            (anchorJump ? section->getPageForAnchor(pendingAnchor).has_value()
                        : target + PARTIAL_REBUILD_START_MARGIN < static_cast<int>(section->pageCount))) {
          LOG_DBG("ERS", "Partial covers target %d of %d; deferring extension build", target, section->pageCount);
        } else {
          const size_t spineBytes =
              epub->getCumulativeSpineItemSize(currentSpineIndex) -
              (currentSpineIndex > 0 ? epub->getCumulativeSpineItemSize(currentSpineIndex - 1) : 0);
          const bool willInflate = !section->hasHtmlCache();
          bool showPopup;
          if (anchorJump) {
            showPopup = !section->findAnchor(pendingAnchor).has_value() && spineBytes > BUILD_POPUP_BYTE_THRESHOLD;
          } else {
            const bool targetAvailable = target < static_cast<int>(section->pageCount);
            showPopup = !targetAvailable && ((spineBytes > BUILD_POPUP_BYTE_THRESHOLD && willInflate) ||
                                             target > BUILD_POPUP_PAGE_THRESHOLD);
          }
          if (showPopup) {
            GUI.drawPopup(renderer, tr(STR_INDEXING));
            pagesUntilFullRefresh = 1;
          }
          buildPopupPending = !showPopup;
          const unsigned long buildStartMs = millis();
          bool started;
          {
            GfxRenderer::FrameBufferLoan loan(renderer);
            started = section->startBuild(renderSpec, [this] { showBuildPopup(renderer, pagesUntilFullRefresh); });
          }
          if (!started) {
            LOG_ERR("ERS", "Failed to start section build");
            section.reset();
            buildPopupPending = false;
            showBuildError();
            return;
          }
          while (!section->isBuildComplete() &&
                 (anchorJump               ? !section->findAnchor(pendingAnchor)
                  : offsetJump.has_value() ? !section->buildReachedVisibleTextOffset(*offsetJump)
                                           : static_cast<int>(section->pageCount) <= target)) {
            if (buildPopupPending && millis() - buildStartMs >= BUILD_POPUP_DEADLINE_MS) {
              showBuildPopup(renderer, pagesUntilFullRefresh);
            }
            if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
              LOG_ERR("ERS", "Failed during incremental section build");
              section.reset();
              buildPopupPending = false;
              showBuildError();
              return;
            }
          }
          buildPopupPending = false;
        }
      }
    } else {
      LOG_DBG("ERS", "Cache found, skipping build...");
    }

    if (pendingPageJump.has_value()) {
      section->currentPage = *pendingPageJump;
      pendingPageJump.reset();
    } else {
      section->currentPage = nextPageNumber;
      if (section->currentPage < 0) section->currentPage = 0;
    }

    if (offsetJump.has_value()) {
      if (const auto offsetPage = section->getPageForVisibleTextOffset(*offsetJump)) {
        section->currentPage = *offsetPage;
        clearDeferredReposition();
      }
    }
    if (explicitOffsetJump) {
      clearDeferredReposition();
    }
    pendingOffsetJump.reset();

    if (!pendingAnchor.empty()) {
      const auto page = section->findAnchor(pendingAnchor);
      if (page) {
        section->currentPage = *page;
        LOG_DBG("ERS", "Resolved anchor '%s' to page %d", pendingAnchor.c_str(), *page);
      }
      pendingAnchor.clear();
    }

    if (pendingPercentJump && section->pageCount > 0) {
      int newPage = static_cast<int>(pendingSpineProgress * static_cast<float>(section->pageCount));
      if (newPage >= section->pageCount) newPage = section->pageCount - 1;
      section->currentPage = newPage;
      pendingPercentJump = false;
    }
  }

  if (section->isPartial() && section->currentPage >= static_cast<int>(section->pageCount)) {
    GUI.drawPopup(renderer, tr(STR_INDEXING));
    pagesUntilFullRefresh = 1;
  }
  while (section->isPartial() && section->currentPage >= static_cast<int>(section->pageCount)) {
    if (!section->isBuilding() && !section->startBuild(renderSpec)) {
      LOG_ERR("ERS", "Failed to start partial extension build");
      section.reset();
      showBuildError();
      return;
    }
    while (!section->isBuildComplete() && section->currentPage >= static_cast<int>(section->pageCount)) {
      if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
        LOG_ERR("ERS", "Failed during incremental section build");
        section.reset();
        showBuildError();
        return;
      }
    }
  }
  if (section->isBuilding()) {
    while (!section->isBuildComplete() && section->currentPage >= static_cast<int>(section->pageCount)) {
      if (!section->buildSomeMore(BUILD_PAGES_PER_CHUNK)) {
        LOG_ERR("ERS", "Failed during incremental section build");
        section.reset();
        showBuildError();
        return;
      }
    }
  }

  if (!section->isBuilding() && section->pageCount > 0 &&
      section->currentPage >= static_cast<int>(section->pageCount)) {
    section->currentPage = section->pageCount - 1;
  }

  applyDeferredReposition();

  renderer.clearScreen();

  if (section->pageCount == 0) {
    LOG_DBG("ERS", "No pages to render");
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_CHAPTER), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  if (section->currentPage < 0 || section->currentPage >= section->pageCount) {
    LOG_DBG("ERS", "Page out of bounds: %d (max %d)", section->currentPage, section->pageCount);
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_OUT_OF_BOUNDS), true, EpdFontFamily::BOLD);
    renderStatusBar();
    renderer.displayBuffer();
    automaticPageTurnActive = false;
    showPendingSyncSaveError();
    return;
  }

  updateBookmarkFlag();

  {
    auto p = section->loadPage(section->currentPage);
    if (!p) {
      LOG_ERR("ERS", "Failed to load page from SD - clearing section cache");
      automaticPageTurnActive = false;
      const bool giveUp = ++pageLoadRetryCount > MAX_PAGE_LOAD_RETRIES;
      section->abandonBuild();
      section->clearCache();
      section.reset();
      if (giveUp) {
        LOG_ERR("ERS", "Page load retry limit reached, aborting");
        pageLoadRetryCount = 0;
        renderer.clearScreen();
        renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
        renderer.displayBuffer();
        showPendingSyncSaveError();
        return;
      }
      requestUpdate();
      showPendingSyncSaveError();
      return;
    }
    pageLoadRetryCount = 0;

    currentPageVisibleOffset = p->visibleTextOffset;
    currentPageFootnotes = std::move(p->footnotes);

    const auto start = millis();
    renderContents(std::move(p), orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft);
    LOG_DBG("ERS", "Rendered page in %dms", millis() - start);
    lastRenderCompleteMs = millis();
  }

  if (currentSpineIndex != lastSavedSpineIndex || section->currentPage != lastSavedPage ||
      section->pageCount != lastSavedPageCount) {
    if (saveProgress(currentSpineIndex, section->currentPage, section->estimatedTotalPages())) {
      lastSavedSpineIndex = currentSpineIndex;
      lastSavedPage = section->currentPage;
      lastSavedPageCount = section->estimatedTotalPages();
    }
  }

  showPendingSyncSaveError();

  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }

  if (showBookmarkMessage) {
    GUI.drawPopup(renderer, bookmarkRemoved ? tr(STR_BOOKMARK_REMOVED) : tr(STR_BOOKMARK_ADDED));
  }

  if (showDictionaryMessage) {
    GUI.drawPopup(renderer, tr(STR_DICT_NO_DICT_SET));
  }
}

void EpubReaderActivity::onEndOfBookRendered() {
  automaticPageTurnActive = false;
  if (pendingSyncSaveError) {
    pendingSyncSaveError = false;
    GUI.drawPopup(renderer, tr(STR_SAVE_PROGRESS_FAILED));
  }
}

bool EpubReaderActivity::applyDeferredReposition() {
  if ((!cachedVisibleTextOffset.has_value() && cachedChapterTotalPageCount == 0) || !section || section->isBuilding()) {
    return false;
  }
  bool changed = false;
  if (currentSpineIndex == cachedSpineIndex) {
    int newPage = section->currentPage;
    bool mappedOffset = false;
    if (cachedVisibleTextOffset.has_value()) {
      if (const auto offsetPage = section->getPageForVisibleTextOffset(*cachedVisibleTextOffset)) {
        newPage = *offsetPage;
        mappedOffset = true;
      }
    }
    if (!mappedOffset && cachedChapterTotalPageCount > 0 && section->pageCount != cachedChapterTotalPageCount) {
      const float progress = static_cast<float>(section->currentPage) / static_cast<float>(cachedChapterTotalPageCount);
      newPage = static_cast<int>(progress * static_cast<float>(section->pageCount));
    }
    if (newPage < 0) newPage = 0;
    if (section->pageCount > 0 && newPage >= static_cast<int>(section->pageCount)) {
      newPage = section->pageCount - 1;
    }
    if (newPage != section->currentPage) {
      section->currentPage = newPage;
      changed = true;
    }
  }
  clearDeferredReposition();
  return changed;
}

void EpubReaderActivity::clearDeferredReposition() {
  cachedChapterTotalPageCount = 0;
  cachedVisibleTextOffset.reset();
}

bool EpubReaderActivity::saveProgress(int spineIndex, int currentPage, int pageCount) {
  std::optional<uint32_t> offset;
  if (section && spineIndex == currentSpineIndex && currentPage >= 0 && currentPage < section->pageCount) {
    offset = (currentPage == section->currentPage && currentPageVisibleOffset.has_value())
                 ? currentPageVisibleOffset
                 : section->getVisibleTextOffsetForPage(static_cast<uint16_t>(currentPage));
  }
  return EpubReaderUtils::saveProgress(*epub, spineIndex, currentPage, pageCount, offset);
}

void EpubReaderActivity::rememberCurrentContentOffset() {
  cachedVisibleTextOffset.reset();
  if (section && section->currentPage >= 0 && section->currentPage < section->pageCount) {
    cachedVisibleTextOffset = section->getVisibleTextOffsetForPage(static_cast<uint16_t>(section->currentPage));
  }
}

void EpubReaderActivity::renderContents(std::unique_ptr<Page> page, const int orientedMarginTop,
                                        const int orientedMarginRight, const int orientedMarginBottom,
                                        const int orientedMarginLeft) {
  const auto t0 = millis();
  const int fontId = SETTINGS.getReaderFontId();

  struct PxcSlotGuard {
    ~PxcSlotGuard() { ImageBlock::releaseRenderCache(); }
  } pxcSlotGuard;

  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
  scope.endScanAndPrewarm();
  const auto tPrewarm = millis();

  const bool pageHasImages = page->hasImages();
  const bool pageHasImagesNeedingDecode = pageHasImages && page->hasImagesNeedingDecode();
  const bool manualRefreshPending = forcedRefreshPending;
  forcedRefreshPending = false;
  const bool cleanImageBasePending = manualRefreshPending || pagesUntilFullRefresh <= 1;
  const bool needsTextGrayscale = SETTINGS.textAntiAliasing;
  const bool needsAnyGrayscale = needsTextGrayscale || pageHasImages;
  const bool tiledGrayscale = needsAnyGrayscale && renderer.supportsStripGrayscale();
  // Paper Mono only (no other panel combines): defer the B/W base activation so
  // the gray planes join it in a single waveform. Displaying the base
  // separately makes the gray pass re-drive the whole text body — a visible
  // flash on every AA page.
  const bool combinedGrayscaleBase = tiledGrayscale && !pageHasImages && renderer.combinesGrayscaleBase();
  const bool overlapRefresh = tiledGrayscale && renderer.supportsAsyncRefresh() && !pageHasImages;
  auto renderGrayscalePass = [&]() {
    if (needsTextGrayscale) {
      page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    } else {
      page->renderImages(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    }
  };

  if (pageHasImagesNeedingDecode) {
    page->renderWithImagePlaceholders(renderer, fontId, orientedMarginLeft, orientedMarginTop);
    renderStatusBar();
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    renderer.clearScreen();
  }

  page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
  renderStatusBar();
  const auto tBwRender = millis();

  if (pageHasImages) {
    int16_t imgX, imgY, imgW, imgH;
    if (page->getImageBoundingBox(imgX, imgY, imgW, imgH)) {
      if (cleanImageBasePending) {
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      }
      renderer.fillRect(imgX + orientedMarginLeft, imgY + orientedMarginTop, imgW, imgH, false);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);

      page->render(renderer, fontId, orientedMarginLeft, orientedMarginTop);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    } else {
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
    pagesUntilFullRefresh = 1;
  } else if (combinedGrayscaleBase) {
    // Stash the base without activating; displayGrayBuffer() below commits
    // base + grays as one waveform.
    ReaderUtils::displayBaseWithRefreshCycle(renderer, pagesUntilFullRefresh);
  } else {
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh, overlapRefresh);
  }
  const auto tDisplay = millis();

  if (tiledGrayscale) {
    constexpr int STRIP_ROWS = 80;
    const int gh = renderer.getDisplayHeight();
    const int gwBytes = renderer.getDisplayWidthBytes();
    const size_t planeBytes = static_cast<size_t>(gwBytes) * gh;

    auto renderPlaneToBuffer = [&](const bool lsbPlane, uint8_t* buf) {
      renderer.setRenderMode(lsbPlane ? GfxRenderer::GRAYSCALE_LSB : GfxRenderer::GRAYSCALE_MSB);
      for (int y = 0; y < gh; y += STRIP_ROWS) {
        const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
        renderer.beginStripTarget(buf + static_cast<size_t>(y) * gwBytes, y, rows);
        renderer.clearScreen(0x00);
        renderGrayscalePass();
        renderer.endStripTarget();
      }
    };

    constexpr size_t PLANE_BUF_HEADROOM = 60000;
    constexpr size_t PLANE_BUF_MAX_ALLOC_RESERVE = 16 * 1024;
    const auto planeBufFits = [planeBytes] {
      return ESP.getFreeHeap() >= planeBytes + PLANE_BUF_HEADROOM &&
             ESP.getMaxAllocHeap() >= planeBytes + PLANE_BUF_MAX_ALLOC_RESERVE;
    };
    auto lsbPlaneBuf = (overlapRefresh && planeBufFits()) ? makeUniqueNoThrow<uint8_t[]>(planeBytes) : nullptr;
    auto msbPlaneBuf = (lsbPlaneBuf && planeBufFits()) ? makeUniqueNoThrow<uint8_t[]>(planeBytes) : nullptr;

    if (lsbPlaneBuf) {
      renderPlaneToBuffer(true, lsbPlaneBuf.get());
      if (msbPlaneBuf) renderPlaneToBuffer(false, msbPlaneBuf.get());
      const auto tGrayRender = millis();

      renderer.waitRefreshComplete();
      const auto tWait = millis();

      renderer.writeGrayscalePlaneStrip(true, lsbPlaneBuf.get(), 0, gh);
      if (msbPlaneBuf) {
        renderer.writeGrayscalePlaneStrip(false, msbPlaneBuf.get(), 0, gh);
      } else {
        renderPlaneToBuffer(false, lsbPlaneBuf.get());
        renderer.writeGrayscalePlaneStrip(false, lsbPlaneBuf.get(), 0, gh);
      }
      const auto tGrayWrite = millis();

      renderer.setRenderMode(GfxRenderer::BW);
      renderer.displayGrayBuffer();
      const auto tGrayDisplay = millis();

      renderer.cleanupGrayscaleWithFrameBuffer();
      const auto tEnd = millis();

      LOG_DBG("ERS",
              "Page render (tiled async): prewarm=%lums bw_render=%lums display=%lums gray_render=%lums "
              "wait=%lums gray_write=%lums gray_display=%lums cleanup=%lums total=%lums (planes buffered: %d)",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tGrayRender - tDisplay, tWait - tGrayRender,
              tGrayWrite - tWait, tGrayDisplay - tGrayWrite, tEnd - tGrayDisplay, tEnd - t0, msbPlaneBuf ? 2 : 1);
    } else {
      auto scratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(gwBytes) * STRIP_ROWS);
      renderer.waitRefreshComplete();
      if (!scratch) {
        LOG_ERR("ERS", "OOM: grayscale strip scratch (%d bytes); skipping AA this page", gwBytes * STRIP_ROWS);
        if (overlapRefresh || combinedGrayscaleBase) {
          // The BW refresh ran the shadow-free async path, so controller RAM's
          // differential baseline was never rebuilt. Even with AA skipped it must
          // be re-synced from the intact BW framebuffer, or the next differential
          // update diffs against stale contents. On the combined-base path the
          // base activation is still deferred; this cleanup commits it so the
          // page reaches the panel even without its grays.
          renderer.cleanupGrayscaleWithFrameBuffer();
        }
      } else {
        renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
        for (int y = 0; y < gh; y += STRIP_ROWS) {
          const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
          renderer.beginStripTarget(scratch.get(), y, rows);
          renderer.clearScreen(0x00);
          renderGrayscalePass();
          renderer.endStripTarget();
          renderer.writeGrayscalePlaneStrip(true, scratch.get(), y, rows);
        }
        const auto tGrayLsb = millis();

        renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
        for (int y = 0; y < gh; y += STRIP_ROWS) {
          const int rows = (gh - y < STRIP_ROWS) ? (gh - y) : STRIP_ROWS;
          renderer.beginStripTarget(scratch.get(), y, rows);
          renderer.clearScreen(0x00);
          renderGrayscalePass();
          renderer.endStripTarget();
          renderer.writeGrayscalePlaneStrip(false, scratch.get(), y, rows);
        }
        const auto tGrayMsb = millis();

        renderer.setRenderMode(GfxRenderer::BW);
        renderer.displayGrayBuffer();
        const auto tGrayDisplay = millis();

        renderer.cleanupGrayscaleWithFrameBuffer();
        const auto tCleanup = millis();

        const auto tEnd = millis();
        LOG_DBG("ERS",
                "Page render (tiled): prewarm=%lums bw_render=%lums display=%lums gray_lsb=%lums "
                "gray_msb=%lums gray_display=%lums cleanup=%lums total=%lums",
                tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tGrayLsb - tDisplay, tGrayMsb - tGrayLsb,
                tGrayDisplay - tGrayMsb, tCleanup - tGrayDisplay, tEnd - t0);
      }
    }
  } else {
    if (needsAnyGrayscale) {
      if (!renderer.storeBwBuffer()) {
        LOG_ERR("ERS", "Failed to store BW buffer for grayscale render; skipping grayscale this page");
        return;
      }
      const auto tBwStore = millis();

      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      renderGrayscalePass();
      renderer.copyGrayscaleLsbBuffers();
      const auto tGrayLsb = millis();

      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      renderGrayscalePass();
      renderer.copyGrayscaleMsbBuffers();
      const auto tGrayMsb = millis();

      renderer.displayGrayBuffer();
      const auto tGrayDisplay = millis();
      renderer.setRenderMode(GfxRenderer::BW);
      renderer.restoreBwBuffer();
      const auto tBwRestore = millis();

      const auto tEnd = millis();
      LOG_DBG("ERS",
              "Page render: prewarm=%lums bw_render=%lums display=%lums bw_store=%lums "
              "gray_lsb=%lums gray_msb=%lums gray_display=%lums bw_restore=%lums total=%lums",
              tPrewarm - t0, tBwRender - tPrewarm, tDisplay - tBwRender, tBwStore - tDisplay, tGrayLsb - tBwStore,
              tGrayMsb - tGrayLsb, tGrayDisplay - tGrayMsb, tBwRestore - tGrayDisplay, tEnd - t0);
    } else {
      const auto tEnd = millis();
      LOG_DBG("ERS", "Page render: prewarm=%lums bw_render=%lums display=%lums total=%lums", tPrewarm - t0,
              tBwRender - tPrewarm, tDisplay - tBwRender, tEnd - t0);
    }
  }
}

void EpubReaderActivity::renderStatusBar() const {
  const int currentPage = section ? section->currentPage + 1 : 1;
  const float pageCount = section ? section->estimatedTotalPages() : 1;
  const float sectionChapterProg = (pageCount > 0) ? (static_cast<float>(currentPage) / pageCount) : 0;
  const float bookProgress = epub ? (epub->calculateProgress(currentSpineIndex, sectionChapterProg) * 100) : 0;

  std::string title;
  int textYOffset = 0;
  const auto sb = SETTINGS.statusBarSpec();

  if (automaticPageTurnActive) {
    title = tr(STR_AUTO_TURN_ENABLED) + std::to_string(60 * 1000 / pageTurnDuration);
    const uint8_t statusBarHeight = UITheme::getInstance().getStatusBarHeight();
    if (statusBarHeight == 0 || statusBarHeight == UITheme::getInstance().getProgressBarHeight()) {
      textYOffset += UITheme::getInstance().getMetrics().statusBarVerticalMargin;
    }
  } else if (sb.titleMode == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = tr(STR_UNNAMED);
    if (epub) {
      const int tocIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
      if (tocIndex != -1) {
        const auto tocItem = epub->getTocItem(tocIndex);
        title = tocItem.title;
      }
    }
  } else if (sb.titleMode == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE) {
    title = epub ? epub->getTitle() : "";
  }

  GUI.drawStatusBar(renderer, bookProgress, currentPage, pageCount, title, 0, textYOffset, true, currentPageBookmarked,
                    section ? section->isBuilding() : false);
}

void EpubReaderActivity::navigateToHref(const std::string& hrefStr, const bool savePosition) {
  if (!epub) return;

  if (savePosition && section && footnoteDepth < MAX_FOOTNOTE_DEPTH) {
    savedPositions[footnoteDepth] = {currentSpineIndex, section->currentPage};
    footnoteDepth++;
    LOG_DBG("ERS", "Saved position [%d]: spine %d, page %d", footnoteDepth, currentSpineIndex, section->currentPage);
  }

  std::string anchor;
  const auto hashPos = hrefStr.find('#');
  if (hashPos != std::string::npos && hashPos + 1 < hrefStr.size()) {
    anchor = hrefStr.substr(hashPos + 1);
  }

  bool sameFile = !hrefStr.empty() && hrefStr[0] == '#';
  int targetSpineIndex = sameFile ? currentSpineIndex : epub->resolveHrefToSpineIndex(hrefStr);

  if (targetSpineIndex < 0) {
    LOG_DBG("ERS", "Could not resolve href: %s", hrefStr.c_str());
    if (savePosition && footnoteDepth > 0) footnoteDepth--;
    return;
  }

  {
    RenderLock lock;
    clearDeferredReposition();
    pendingAnchor = std::move(anchor);
    currentSpineIndex = targetSpineIndex;
    nextPageNumber = 0;
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
    RenderLock lock;
    clearDeferredReposition();
    currentSpineIndex = pos.spineIndex;
    nextPageNumber = pos.pageNumber;
    section.reset();
  }
  requestUpdate();
}

void EpubReaderActivity::loadCachedBookmarks() {
  cachedBookmarks.clear();
  if (cachedBookmarks.capacity() < initialBookmarkCacheCapacity) {
    cachedBookmarks.reserve(initialBookmarkCacheCapacity);
  }
  if (!epub) {
    currentPageBookmarked = false;
    return;
  }

  BookmarkFile::load(epub->getPath(), cachedBookmarks);
  updateBookmarkFlag();
}

void EpubReaderActivity::addBookmark() {
  if (!section || !epub) return;
  LOG_DBG("ERS", "Toggle bookmark at spine %d, page %d", currentSpineIndex, section ? section->currentPage : -1);
  int currentPage;
  int pageCount;
  {
    RenderLock lock;
    pageCount = section->estimatedTotalPages();
    currentPage = section->currentPage;
  }

  SavedProgressPosition progress = ProgressMapper::toSavedProgress(epub, getCurrentPosition());
  const ProgressRange pageRange = getPageProgressRange(epub, currentSpineIndex, currentPage, pageCount);

  const size_t bookmarkCountBeforeToggle = cachedBookmarks.size();
  cachedBookmarks.erase(std::remove_if(cachedBookmarks.begin(), cachedBookmarks.end(),
                                       [&](const BookmarkEntry& b) {
                                         return bookmarkMatchesProgress(b, currentSpineIndex, currentPage, pageCount,
                                                                        pageRange);
                                       }),
                        cachedBookmarks.end());
  if (cachedBookmarks.size() != bookmarkCountBeforeToggle) {
    bookmarkRemoved = true;
    currentPageBookmarked = false;
  } else {
    std::string pageText;
    if (currentPage >= 0 && currentPage < pageCount) {
      pageText = section->getTextFromSectionFile();
    }
    BookmarkEntry entry;
    entry.percentage = progress.percentage;
    entry.xpath = progress.xpath;
    entry.summary = BookmarkUtil::sanitizeBookmarkSummary(pageText);
    entry.computedSpineIndex = currentSpineIndex;
    entry.computedChapterPageCount = pageCount;
    entry.computedChapterProgress = currentPage;
    const std::optional<uint32_t> offset =
        currentPageVisibleOffset.has_value() ? currentPageVisibleOffset
        : (currentPage >= 0 && currentPage < section->pageCount)
            ? section->getVisibleTextOffsetForPage(static_cast<uint16_t>(currentPage))
            : std::nullopt;
    if (offset.has_value()) {
      entry.visibleTextOffset = *offset;
      entry.hasVisibleTextOffset = true;
    }
    cachedBookmarks.insert(cachedBookmarks.begin(), entry);
    bookmarkRemoved = false;
    currentPageBookmarked = true;
  }

  if (!BookmarkFile::save(epub->getPath(), cachedBookmarks)) {
    LOG_ERR("ERS", "Failed to save bookmarks");
  }
  requestUpdate();
}

void EpubReaderActivity::updateBookmarkFlag() {
  if (!section || !epub || cachedBookmarks.empty()) {
    currentPageBookmarked = false;
    return;
  }
  const int pageCount = section->estimatedTotalPages();
  const ProgressRange pageRange = getPageProgressRange(epub, currentSpineIndex, section->currentPage, pageCount);
  currentPageBookmarked = std::any_of(cachedBookmarks.begin(), cachedBookmarks.end(), [&](const BookmarkEntry& b) {
    return bookmarkMatchesProgress(b, currentSpineIndex, section->currentPage, pageCount, pageRange);
  });
}

ScreenshotInfo EpubReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Epub;
  if (epub) {
    snprintf(info.title, sizeof(info.title), "%s", epub->getTitle().c_str());
    info.spineIndex = currentSpineIndex;
  }
  if (section) {
    info.currentPage = section->currentPage + 1;
    info.totalPages = section->estimatedTotalPages();
    if (epub && epub->getBookSize() > 0 && info.totalPages > 0) {
      const float chapterProgress = static_cast<float>(section->currentPage) / static_cast<float>(info.totalPages);
      int pct = static_cast<int>(epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f + 0.5f);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
      info.progressPercent = pct;
    }
  }
  return info;
}

CrossPointPosition EpubReaderActivity::getCurrentPosition() const {
  const int currentPage = section ? section->currentPage : nextPageNumber;
  const int totalPages = section ? section->estimatedTotalPages() : cachedChapterTotalPageCount;
  std::optional<uint16_t> paragraphIndex;
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    const uint16_t paragraphPage =
        currentPage > 0 ? static_cast<uint16_t>(currentPage - 1) : static_cast<uint16_t>(currentPage);
    if (const auto pIdx = section->getParagraphIndexForPage(paragraphPage)) {
      paragraphIndex = *pIdx;
    }
  }

  CrossPointPosition localPos = {currentSpineIndex, currentPage, totalPages};
  if (section && currentPage >= 0 && currentPage < section->pageCount) {
    if (const auto offset = section->getVisibleTextOffsetForPage(static_cast<uint16_t>(currentPage))) {
      localPos.visibleTextOffset = *offset;
      localPos.hasVisibleTextOffset = true;
    }
  }
  if (paragraphIndex.has_value()) {
    localPos.paragraphIndex = *paragraphIndex;
    localPos.hasParagraphIndex = true;
  }
  return localPos;
}
