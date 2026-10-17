#include "FinishedBookActivity.h"

#include <Bitmap.h>
#include <BuildArena.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <HalSystem.h>  // feedWatchdog()
#include <I18n.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <Memory.h>
#include <PngToBmpConverter.h>
#include <SidecarFiles.h>
#include <Txt.h>
#include <Xtc.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "BookRelocation.h"
#include "KOReaderCredentialStore.h"
#include "OpdsServerStore.h"
#include "ReaderActivity.h"
#include "ReadingSessionTracker.h"
#include "RecentBooksStore.h"
#include "activities/ActivityManager.h"
#include "components/CoverGridLayout.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
std::string extractFolderPath(const std::string& filePath) {
  const auto lastSlash = filePath.find_last_of('/');
  if (lastSlash == std::string::npos || lastSlash == 0) {
    return "/";
  }
  return filePath.substr(0, lastSlash);
}

std::string getFilename(const std::string& filePath) {
  const auto lastSlash = filePath.find_last_of('/');
  if (lastSlash == std::string::npos) {
    return filePath;
  }
  return filePath.substr(lastSlash + 1);
}

static constexpr int kFinishedBookCoverHeight = CoverGridLayout::kThumbHeight;
static constexpr int kFinishedBookCoverMaxWidth = CoverGridLayout::kThumbWidth;

std::string findUniquePathWithSuffix(const std::string& basePath) {
  if (!Storage.exists(basePath.c_str())) {
    return basePath;
  }

  const auto dotPos = basePath.find_last_of('.');
  const std::string base = (dotPos == std::string::npos) ? basePath : basePath.substr(0, dotPos);
  const std::string ext = (dotPos == std::string::npos) ? std::string() : basePath.substr(dotPos);
  for (int suffix = 1; suffix < 1000; ++suffix) {
    const std::string candidate = base + " (" + std::to_string(suffix) + ")" + ext;
    if (!Storage.exists(candidate.c_str())) {
      return candidate;
    }
  }
  return {};
}

std::string findUniqueCompletedSidecarPath(const std::string& basePath) { return findUniquePathWithSuffix(basePath); }

std::string convertSidecarToBmp(const std::string& cacheDir, const std::string& sidecarPath, int width, int height,
                                const std::string& fileName, BuildArena* scratch) {
  if (!Storage.exists(cacheDir.c_str())) {
    Storage.mkdir(cacheDir.c_str());
  }
  const std::string bmpPath = cacheDir + "/" + fileName;
  if (Storage.exists(bmpPath.c_str())) {
    return bmpPath;
  }

  FsFile src;
  if (!Storage.openFileForRead("FIN", sidecarPath, src)) {
    return "";
  }
  FsFile dst;
  if (!Storage.openFileForWrite("FIN", bmpPath, dst)) {
    src.close();
    return "";
  }

  bool ok = false;
  if (FsHelpers::hasJpgExtension(sidecarPath)) {
    ok = JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(src, dst, width, height, scratch,
                                                             CoverGridLayout::kThumbCrop);
  } else if (FsHelpers::hasPngExtension(sidecarPath)) {
    ok = PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(src, dst, width, height, CoverGridLayout::kThumbCrop);
  } else if (FsHelpers::hasBmpExtension(sidecarPath)) {
    uint8_t buffer[1024];
    while (src.available()) {
      size_t bytesRead = src.read(buffer, sizeof(buffer));
      dst.write(buffer, bytesRead);
    }
    ok = true;
  }

  src.close();
  dst.close();
  if (!ok) {
    Storage.remove(bmpPath.c_str());
    return "";
  }
  return bmpPath;
}

std::string getSidecarCoverBmpPath(const std::string& bookPath, int width, int height, BuildArena* scratch) {
  const std::string sidecarPath = ReaderActivity::sidecarCoverPath(bookPath);
  if (sidecarPath.empty()) {
    return "";
  }

  if (FsHelpers::hasBmpExtension(sidecarPath)) {
    return sidecarPath;
  }

  const std::string fileName = "thumb_" + std::to_string(width) + "x" + std::to_string(height) + ".bmp";
  return convertSidecarToBmp(ReaderActivity::bookCacheDir(bookPath), sidecarPath, width, height, fileName, scratch);
}

// A sidecar is bound to its book by filename, so every one of them has to
// travel with it - a book that arrives in /COMPLETED without its sidecars
// silently loses its cover and reverts to the metadata embedded in the EPUB.
// Which extensions those are is SidecarFiles' business, not this function's.
bool moveSidecarFilesToCompleted(const std::string& currentBookPath, const std::string& targetBookPath) {
  const std::string srcBase = SidecarFiles::basePath(currentBookPath);
  const std::string dstBase = SidecarFiles::basePath(targetBookPath);
  if (srcBase.empty() || dstBase.empty()) {
    return false;
  }

  bool success = true;
  for (const char* ext : SidecarFiles::existingExtensions(currentBookPath)) {
    const std::string srcSidecar = srcBase + ext;
    std::string dstSidecar = dstBase + ext;
    if (Storage.exists(dstSidecar.c_str())) {
      dstSidecar = findUniqueCompletedSidecarPath(dstSidecar);
      if (dstSidecar.empty()) {
        LOG_ERR("FIN", "Failed to create unique sidecar target for %s", srcSidecar.c_str());
        success = false;
        continue;
      }
    }

    if (!Storage.rename(srcSidecar.c_str(), dstSidecar.c_str())) {
      LOG_ERR("FIN", "Failed to move sidecar %s -> %s", srcSidecar.c_str(), dstSidecar.c_str());
      success = false;
    }
  }
  return success;
}

struct NextBookMetadata {
  std::string title;
  std::string author;
  std::string series;
  std::string coverPath;
};

// `scratch` (may be null) backs the cover decoders; see LentCoverScratch.
NextBookMetadata loadNextBookMetadata(const std::string& nextBookPath, BuildArena* scratch) {
  NextBookMetadata metadata;
  if (nextBookPath.empty()) {
    return metadata;
  }

  if (FsHelpers::hasEpubExtension(nextBookPath)) {
    Epub epub(nextBookPath, "/.crosspoint");
    epub.setSyntheticTocFallbackEnabled(SETTINGS.syntheticTocFallback != 0);
    // loadForCover(), not load(): this preview needs metadata + the cover thumb, never the spine/TOC.
    // A full load() here rebuilt book.bin and reparsed the CSS for a book the user may not even open,
    // costing ~2 s inside the reader — the same waste the sequel scan used to pay per candidate.
    if (epub.loadForCover()) {
      metadata.title = epub.getTitle();
      metadata.author = epub.getAuthor();
      metadata.series = epub.getSeries();
      if (!metadata.series.empty() && !epub.getSeriesIndex().empty()) {
        metadata.series += " #" + epub.getSeriesIndex();
      }
      if (epub.generateThumbBmp(kFinishedBookCoverMaxWidth, kFinishedBookCoverHeight, /*allowExtract=*/true, scratch,
                                CoverGridLayout::kThumbCrop) == ThumbResult::Ok) {
        metadata.coverPath = epub.getThumbBmpPath(kFinishedBookCoverMaxWidth, kFinishedBookCoverHeight);
      } else {
        metadata.coverPath =
            getSidecarCoverBmpPath(nextBookPath, kFinishedBookCoverMaxWidth, kFinishedBookCoverHeight, scratch);
      }
    }
    return metadata;
  }

  if (FsHelpers::hasXtcExtension(nextBookPath)) {
    Xtc xtc(nextBookPath, "/.crosspoint");
    if (xtc.load()) {
      metadata.title = xtc.getTitle();
      metadata.author = xtc.getAuthor();
      if (xtc.generateThumbBmp(kFinishedBookCoverMaxWidth, kFinishedBookCoverHeight)) {
        metadata.coverPath = xtc.getThumbBmpPath(kFinishedBookCoverMaxWidth, kFinishedBookCoverHeight);
      } else {
        metadata.coverPath =
            getSidecarCoverBmpPath(nextBookPath, kFinishedBookCoverMaxWidth, kFinishedBookCoverHeight, scratch);
      }
    }
    return metadata;
  }

  if (FsHelpers::hasMarkdownExtension(nextBookPath) || FsHelpers::hasTxtExtension(nextBookPath)) {
    Txt txt(nextBookPath, "/.crosspoint");
    if (txt.load()) {
      metadata.title = txt.getTitle();
      if (txt.generateCoverBmp()) {
        metadata.coverPath = txt.getCoverBmpPath();
      } else {
        metadata.coverPath =
            getSidecarCoverBmpPath(nextBookPath, kFinishedBookCoverMaxWidth, kFinishedBookCoverHeight, scratch);
      }
    }
    return metadata;
  }

  return metadata;
}

// The secondary framebuffer, lent to the cover decoders for one preview load. The reader this screen sits on
// still holds its fonts, caches and both framebuffers, and without a scratch region the JPEG decoder asks for
// 40 KB of free heap: on the X3 it found 39,684 B and the next book's cover was skipped. With the region its heap
// floor is 8 KB. The lend is the one HomeActivity and FileBrowserActivity make: the write buffer is brought up to
// the frame on the panel and, on the X4, RED RAM seeded with it, which single-buffer fast diff requires. Nothing
// is drawn from the region, so giving it back cannot fail. Without a secondary buffer to lend (a caller that has
// it lent already), the decoders use the heap as before.
class LentCoverScratch {
 public:
  LentCoverScratch(Activity& activity, GfxRenderer& renderer) : activity_(activity), renderer_(renderer) {
    if (!renderer_.hasSecondaryBuffer()) return;
    RenderLock lock(activity_);
    renderer_.syncWriteBufferFromDisplayed();
    renderer_.syncRedRamFromFrameBuffer();
    size_t size = 0;
    uint8_t* region = renderer_.borrowSecondaryBuffer(&size);
    if (region == nullptr) return;
    arena_ = makeUniqueNoThrow<BuildArena>(region, size);
    if (!arena_ || !arena_->valid()) {
      arena_.reset();
      renderer_.returnSecondaryBuffer();
      return;
    }
    renderer_.setSingleBufferFastDiff(true);
    LOG_INF("FIN", "Lent secondary framebuffer for the next book's cover (%u bytes, free=%lu)",
            static_cast<unsigned>(size), static_cast<unsigned long>(esp_get_free_heap_size()));
  }

  ~LentCoverScratch() {
    if (!arena_) return;
    RenderLock lock(activity_);
    arena_.reset();
    renderer_.returnSecondaryBuffer();
    renderer_.setSingleBufferFastDiff(false);
    LOG_INF("FIN", "Returned secondary framebuffer (free=%lu)", static_cast<unsigned long>(esp_get_free_heap_size()));
  }

  LentCoverScratch(const LentCoverScratch&) = delete;
  LentCoverScratch& operator=(const LentCoverScratch&) = delete;

  BuildArena* arena() const { return arena_.get(); }

 private:
  Activity& activity_;
  GfxRenderer& renderer_;
  std::unique_ptr<BuildArena> arena_;
};

bool isSupportedBookFile(const std::string& fileName) {
  return FsHelpers::hasEpubExtension(fileName) || FsHelpers::hasXtcExtension(fileName) ||
         FsHelpers::hasTxtExtension(fileName) || FsHelpers::hasMarkdownExtension(fileName);
}

bool caseInsensitiveEqual(const std::string& left, const std::string& right) {
  if (left.size() != right.size()) return false;
  for (size_t i = 0; i < left.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(left[i])) != std::tolower(static_cast<unsigned char>(right[i]))) {
      return false;
    }
  }
  return true;
}

bool naturalLess(const std::string& str1, const std::string& str2) {
  const char* s1 = str1.c_str();
  const char* s2 = str2.c_str();
  while (*s1 && *s2) {
    if (std::isdigit(static_cast<unsigned char>(*s1)) && std::isdigit(static_cast<unsigned char>(*s2))) {
      while (*s1 == '0') s1++;
      while (*s2 == '0') s2++;
      int len1 = 0;
      int len2 = 0;
      while (std::isdigit(static_cast<unsigned char>(s1[len1]))) len1++;
      while (std::isdigit(static_cast<unsigned char>(s2[len2]))) len2++;
      if (len1 != len2) return len1 < len2;
      for (int i = 0; i < len1; ++i) {
        if (s1[i] != s2[i]) return s1[i] < s2[i];
      }
      s1 += len1;
      s2 += len2;
    } else {
      const char c1 = std::tolower(static_cast<unsigned char>(*s1));
      const char c2 = std::tolower(static_cast<unsigned char>(*s2));
      if (c1 != c2) return c1 < c2;
      s1++;
      s2++;
    }
  }
  return *s1 == '\0' && *s2 != '\0';
}

std::optional<float> parseSeriesIndex(const std::string& rawIndex) {
  const char* start = rawIndex.c_str();
  char* end = nullptr;
  errno = 0;
  const float value = std::strtof(start, &end);
  if (end == start || errno == ERANGE) {
    return std::nullopt;
  }
  return value;
}

std::string pathWithFilename(const std::string& directory, const std::string& fileName) {
  if (directory.empty() || directory == "/") {
    return std::string("/") + fileName;
  }
  return directory + "/" + fileName;
}

// Finds the next book in the series: the candidate whose series index is the SMALLEST one still
// greater than the current book's.
//
// Streams the directory and keeps only the running best — one path string of state, no file list, no
// sort. That matters more than it looks: this runs inside the reader with the secondary framebuffer
// restored (~10.7 KB largest contiguous block, measured on X4), so materialising and sorting a vector
// of every filename was the single largest allocation on the path. Streaming makes peak memory O(1)
// in the folder size, which removed the need for a heap gate and a listing cap.
//
// Exhaustive over the folder, deliberately and without a candidate cap. Two earlier revisions tried
// to bound the work and both were silently WRONG rather than merely incomplete:
//   - skipping everything before the current file in sorted order breaks whenever filenames disagree
//     with series numbering;
//   - stopping after the first N entries breaks too, because directory order is filesystem order, so
//     "the first N" is arbitrary — in a 30-book folder the real sequel can sit at position 25 and
//     never be examined. That fails precisely on the large series folders this feature is for.
// Exhaustive is affordable because each candidate is a metadata-only parse (~300 ms) rather than the
// full load (~2 s) this used to do — see Epub::loadForMetadata(). A very large folder still costs
// real time; the answer to that is moving the scan off the main loop, not truncating it into a wrong
// answer. The watchdog feed below keeps a long scan from rebooting the device.
std::string findSeriesSequel(const std::string& directory, const std::string& currentFilename,
                             const std::string& currentSeries, const std::string& currentSeriesIndex) {
  const auto currentIndex = parseSeriesIndex(currentSeriesIndex);
  if (!currentIndex.has_value() || currentSeries.empty()) {
    return {};
  }

  auto root = Storage.open(directory.c_str());
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return {};
  }

  std::string bestPath;
  float bestIndex = std::numeric_limits<float>::infinity();
  size_t examined = 0;

  root.rewindDirectory();
  char name[512];
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    const bool isDir = file.isDirectory();
    file.close();
    if (isDir) continue;

    const std::string fileName = name;
    if (fileName.empty() || fileName[0] == '.' || !FsHelpers::hasEpubExtension(fileName)) {
      continue;
    }
    // The current book is in this directory too; skip it rather than spending a parse on a book that
    // can never be its own sequel.
    if (fileName == currentFilename) {
      continue;
    }

    ++examined;

    // The scan runs synchronously in the main loop, so it blocks input and the idle task throughout,
    // and it is unbounded in the folder size. Each candidate is a ZIP open + OPF parse, so a big
    // folder is many seconds — feed the watchdog and let other tasks run between candidates. The
    // pre-fix scan full-loaded every EPUB (~2 s each) with no yield at all, the suspected cause of
    // the issue #104 reboot.
    HalSystem::feedWatchdog();
    yield();

    const std::string candidatePath = pathWithFilename(directory, fileName);
    Epub epub(candidatePath, "/.crosspoint");
    epub.setSyntheticTocFallbackEnabled(SETTINGS.syntheticTocFallback != 0);
    // Metadata-only: no spine/TOC book.bin build, no manifest index, no CSS parse.
    if (!epub.loadForMetadata()) {
      continue;
    }
    if (!caseInsensitiveEqual(epub.getSeries(), currentSeries)) {
      continue;
    }
    const auto candidateIndex = parseSeriesIndex(epub.getSeriesIndex());
    if (!candidateIndex.has_value() || candidateIndex.value() <= currentIndex.value()) {
      continue;
    }
    if (candidateIndex.value() < bestIndex) {
      bestIndex = candidateIndex.value();
      bestPath = candidatePath;
    }
  }
  root.close();

  LOG_DBG("FIN", "Series-sequel scan: examined %zu candidate(s), next=%s", examined,
          bestPath.empty() ? "(none)" : bestPath.c_str());
  return bestPath;
}

// Finds the book immediately after currentFilename in natural filename order — i.e. the SMALLEST
// filename still greater than the current one.
//
// Like findSeriesSequel, this streams the directory keeping only the running best rather than listing
// and sorting every entry, so peak memory is two short strings regardless of folder size. Opens no
// files at all: filenames alone decide the answer.
std::string findNextAlphabeticalBook(const std::string& directory, const std::string& currentFilename) {
  auto root = Storage.open(directory.c_str());
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return {};
  }

  std::string best;
  root.rewindDirectory();
  char name[512];
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    const bool isDir = file.isDirectory();
    file.close();
    if (isDir) continue;

    const std::string fileName = name;
    if (fileName.empty() || fileName[0] == '.' || !isSupportedBookFile(fileName)) {
      continue;
    }
    // Strictly after the current book, and the earliest such name seen so far.
    if (!naturalLess(currentFilename, fileName)) {
      continue;
    }
    if (best.empty() || naturalLess(fileName, best)) {
      best = fileName;
    }
  }
  root.close();

  if (best.empty()) return {};
  return pathWithFilename(directory, best);
}

bool pathIsInCompleted(const std::string& bookPath) {
  return bookPath.rfind("/COMPLETED/", 0) == 0 || bookPath == "/COMPLETED";
}

std::string buildCompletedTargetPath(const std::string& currentBookPath) {
  const std::string fileName = getFilename(currentBookPath);
  return std::string("/COMPLETED/") + fileName;
}

std::string findUniqueCompletedPath(const std::string& basePath) { return findUniquePathWithSuffix(basePath); }
}  // namespace

namespace BookFinished {

std::string findNextBookInDirectory(const std::string& currentBookPath, const std::string& currentBookSeries,
                                    const std::string& currentBookSeriesIndex) {
  const std::string directory = extractFolderPath(currentBookPath);
  const std::string currentFilename = getFilename(currentBookPath);

  if (!currentBookSeries.empty() && !currentBookSeriesIndex.empty()) {
    const std::string sequel = findSeriesSequel(directory, currentFilename, currentBookSeries, currentBookSeriesIndex);
    if (!sequel.empty() && sequel != currentBookPath) {
      return sequel;
    }
  }

  return findNextAlphabeticalBook(directory, currentFilename);
}

bool moveFinishedBookToCompleted(const std::string& currentBookPath, std::string& outMovedPath,
                                 const bool openInReader) {
  if (pathIsInCompleted(currentBookPath)) {
    outMovedPath = currentBookPath;
    return true;
  }

  const std::string completedDir = "/COMPLETED";
  if (!Storage.exists(completedDir.c_str()) && !Storage.mkdir(completedDir.c_str())) {
    LOG_ERR("FIN", "Failed to create /COMPLETED directory");
    return false;
  }

  std::string targetPath = buildCompletedTargetPath(currentBookPath);
  if (Storage.exists(targetPath.c_str())) {
    targetPath = findUniqueCompletedPath(targetPath);
    if (targetPath.empty()) {
      LOG_ERR("FIN", "Cannot resolve unique /COMPLETED filename");
      return false;
    }
  }

  if (!Storage.rename(currentBookPath.c_str(), targetPath.c_str())) {
    LOG_ERR("FIN", "Failed to move book to /COMPLETED: %s -> %s", currentBookPath.c_str(), targetPath.c_str());
    return false;
  }

  if (!moveSidecarFilesToCompleted(currentBookPath, targetPath)) {
    LOG_ERR("FIN", "One or more sidecar files failed to move for %s", currentBookPath.c_str());
  }
  // Without this the book reopened from /COMPLETED at its first page with no bookmarks, its cache
  // stayed behind under the old path, and a kept recents row pointed at a missing file.
  // Based on crosspoint-reader PR #3354 (Sylve, @s0lness).
  if (openInReader) {
    BookRelocation::bookMovedWhileOpen(currentBookPath, targetPath);
  } else {
    BookRelocation::bookMoved(currentBookPath, targetPath);
  }

  outMovedPath = targetPath;
  return true;
}

std::string applyFinishedBookSettings(const std::string& bookPath, const bool openInReader) {
  // Forget first: the move carries a recents row over to the new path.
  if (SETTINGS.removeFinishedBooksFromRecents) {
    RECENT_BOOKS.removeBook(bookPath);
  }
  std::string movedPath;
  if (SETTINGS.moveFinishedBooksToCompleted && moveFinishedBookToCompleted(bookPath, movedPath, openInReader)) {
    return movedPath;
  }
  return bookPath;
}

void launchFinishedBookFlow(Activity& host, GfxRenderer& renderer, MappedInputManager& mappedInput,
                            const std::string& bookPath, const std::string& series, const std::string& seriesIndex,
                            const std::string& author, void (*onMenuClosed)(void*), void* onMenuClosedCtx,
                            bool (*onSyncToKOReader)(void*, const std::string&, KOReaderSyncPostAction,
                                                     const std::string&),
                            void* onSyncToKOReaderCtx) {
  const std::string nextBookPath = findNextBookInDirectory(bookPath, series, seriesIndex);
  const bool koReaderSyncAvailable = onSyncToKOReader != nullptr && KOREADER_STORE.hasCredentials();
  Activity* hostPtr = &host;
  host.startActivityForResult(
      std::make_unique<FinishedBookActivity>(renderer, mappedInput, bookPath, nextBookPath, author,
                                             koReaderSyncAvailable),
      [hostPtr, bookPath, nextBookPath, author, onMenuClosed, onMenuClosedCtx, onSyncToKOReader,
       onSyncToKOReaderCtx](const ActivityResult& result) {
        if (onMenuClosed) {
          onMenuClosed(onMenuClosedCtx);
        }
        if (result.isCancelled) {
          hostPtr->requestUpdate();
          return;
        }
        // User confirmed they're done with this book — credit a finish to the
        // in-flight session before any tear-down side effects.
        globalReadingSessionTracker().markFinished();
        const auto& menuResult = std::get<MenuResult>(result.data);
        const bool goHome = menuResult.action == static_cast<int>(FinishedBookAction::GoHome);
        const bool openNext =
            menuResult.action == static_cast<int>(FinishedBookAction::OpenNextBook) && !nextBookPath.empty();
        const bool searchOpds =
            menuResult.action == static_cast<int>(FinishedBookAction::SearchOpdsForAuthor) && !author.empty();
        if (!goHome && !openNext && !searchOpds) {
          hostPtr->requestUpdate();
          return;
        }
        // The move-to-/COMPLETED and forget-book settings apply no matter which of the three
        // actions below was picked, and no matter whether the sync toggle is also on — the
        // toggle composes with the picked action rather than replacing it.
        const std::string effectiveBookPath = applyFinishedBookSettings(bookPath, true);
        const bool syncToKOReader =
            onSyncToKOReader != nullptr && KOREADER_STORE.hasCredentials() && SETTINGS.syncFinishedBookToKOReader;
        if (syncToKOReader) {
          // The sync callback (EpubReaderActivity::launchKOReaderSync) hands off to
          // KOReaderSyncActivity, which owns pushing progress and, once its reboot completes,
          // performing the very action (Home / open next book / OPDS search) that was picked
          // here — see the postAction/target comment on launchFinishedBookFlow's declaration.
          KOReaderSyncPostAction postAction = KOReaderSyncPostAction::Home;
          std::string target;
          if (openNext) {
            postAction = KOReaderSyncPostAction::OpenBook;
            target = nextBookPath;
          } else if (searchOpds) {
            postAction = KOReaderSyncPostAction::OpdsSearch;
            target = author;
          }
          // Falls through to the ordinary navigation below when the callback declines, so a
          // sync that cannot run degrades to "the toggle was off" rather than stranding the user.
          if (onSyncToKOReader(onSyncToKOReaderCtx, effectiveBookPath, postAction, target)) {
            return;
          }
        }
        if (goHome) {
          activityManager.goHome();
        } else if (openNext) {
          activityManager.goToReader(nextBookPath);
        } else {
          activityManager.goToBrowserWithSearch(author);
        }
      });
}

}  // namespace BookFinished

namespace fui = freeink::ui;

FinishedBookActivity::FinishedBookActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           std::string currentBookPath, std::string nextBookPath,
                                           std::string currentBookAuthor, bool koReaderSyncAvailable)
    : UiListActivity("FinishedBook", renderer, mappedInput),
      currentBookPath_(std::move(currentBookPath)),
      nextBookPath_(std::move(nextBookPath)),
      currentBookAuthor_(std::move(currentBookAuthor)),
      nextBookAvailable_(!nextBookPath_.empty()),
      nextBookMetadataLoaded_(nextBookPath_.empty()),
      koReaderSyncAvailable_(koReaderSyncAvailable) {
  // Until the preview has loaded, the next-book row names the file.
  if (nextBookAvailable_) nextBookTitle_ = getFilename(nextBookPath_);
}

void FinishedBookActivity::onEnter() {
  rebuildRows();
  UiListActivity::onEnter();
}

// The rows that apply, in display order. None of the conditions changes while the screen is open,
// so this runs once, in onEnter(), before the first render; it allocates only the OPDS label.
void FinishedBookActivity::rebuildRows() {
  int count = 0;
  actions_[count++] = Row::GoHome;
  if (nextBookAvailable_) actions_[count++] = Row::OpenNext;
  if (!currentBookAuthor_.empty() && OPDS_STORE.hasServers()) {
    opdsLabel_.assign(tr(STR_SEARCH_OPDS_FOR_AUTHOR));
    opdsLabel_ += ": ";
    opdsLabel_ += currentBookAuthor_;
    actions_[count++] = Row::SearchOpds;
  }
  if (!pathIsInCompleted(currentBookPath_)) actions_[count++] = Row::ToggleMoveToCompleted;
  actions_[count++] = Row::ToggleForget;
  // A switch, not an action, so it composes with whichever of Home / next book / OPDS search is
  // picked instead of replacing it -- see launchFinishedBookFlow.
  if (koReaderSyncAvailable_) actions_[count++] = Row::ToggleSyncToKOReader;
  actionCount_ = static_cast<uint8_t>(count);
}

const char* FinishedBookActivity::headerTitle() const { return tr(STR_FINISHED_BOOK_HEADER); }

// cppcheck-suppress constParameterCallback ; the signature is fui::ListProps::rowProvider's
void FinishedBookActivity::provideRow(void* ctx, const uint16_t index, fui::ListItem& item) {
  const auto* self = static_cast<const FinishedBookActivity*>(ctx);
  item.actionValue = static_cast<int16_t>(index);
  switch (self->actions_[index]) {
    case Row::GoHome:
      item.label = tr(STR_GO_BACK_TO_HOME);
      item.value = tr(STR_HOME);
      break;
    case Row::OpenNext:
      // The panel above shows the next book's author, series and cover; the row names the book.
      item.label = self->nextBookTitle_.empty() ? tr(STR_OPEN_NEXT_BOOK) : self->nextBookTitle_.c_str();
      item.value = tr(STR_OPEN);
      break;
    case Row::SearchOpds:
      item.label = self->opdsLabel_.c_str();
      item.value = tr(STR_SEARCH);
      break;
    case Row::ToggleMoveToCompleted:
      item.label = tr(STR_MOVE_FINISHED_TO_COMPLETED);
      item.toggle = true;
      item.toggleChecked = SETTINGS.moveFinishedBooksToCompleted != 0;
      break;
    case Row::ToggleForget:
      item.label = tr(STR_FORGET_BOOK);
      item.toggle = true;
      item.toggleChecked = SETTINGS.removeFinishedBooksFromRecents != 0;
      break;
    case Row::ToggleSyncToKOReader:
      item.label = tr(STR_KO_SYNC_FINISHED_BOOK);
      item.toggle = true;
      item.toggleChecked = SETTINGS.syncFinishedBookToKOReader != 0;
      break;
  }
}

void FinishedBookActivity::activateIndex(const int index) {
  if (index < 0 || index >= actionCount_) return;
  switch (actions_[index]) {
    case Row::GoHome:
      finishWith(BookFinished::FinishedBookAction::GoHome);
      return;
    case Row::OpenNext:
      finishWith(BookFinished::FinishedBookAction::OpenNextBook);
      return;
    case Row::SearchOpds:
      finishWith(BookFinished::FinishedBookAction::SearchOpdsForAuthor);
      return;
    case Row::ToggleMoveToCompleted:
      SETTINGS.moveFinishedBooksToCompleted = SETTINGS.moveFinishedBooksToCompleted ? 0 : 1;
      break;
    case Row::ToggleForget:
      SETTINGS.removeFinishedBooksFromRecents = SETTINGS.removeFinishedBooksFromRecents ? 0 : 1;
      break;
    case Row::ToggleSyncToKOReader:
      SETTINGS.syncFinishedBookToKOReader = SETTINGS.syncFinishedBookToKOReader ? 0 : 1;
      break;
  }
  // A switch: the result handler applies it once an action is picked. Saved now, as before, so it
  // is also what the next finished book opens with.
  SETTINGS.saveToFile();
  requestUpdate();
}

void FinishedBookActivity::onBackButton() { finishWith(BookFinished::FinishedBookAction::GoHome); }

void FinishedBookActivity::homeFromList() { onBackButton(); }

void FinishedBookActivity::finishWith(const BookFinished::FinishedBookAction action) {
  app.clearTapFlash();
  MenuResult menuResult;
  menuResult.action = static_cast<int>(action);
  ActivityResult result(menuResult);
  setResult(std::move(result));
  finish();
}

// The next book's preview is read from SD here, on the loop task, once: an OPF parse and maybe a
// cover conversion, which nothing interrupts. It starts only on a tick with no press in flight or
// queued, so it never sits in front of one; presses made during it reach the list on the next tick.
// Returning false hands the tick to the list as usual.
bool FinishedBookActivity::handleCustomInput() {
  if (!nextBookMetadataLoaded_ && !buttonEvents.isGestureInFlight()) loadNextBookPreview();
  return false;
}

void FinishedBookActivity::loadNextBookPreview() {
  NextBookMetadata metadata;
  {
    // Lent only while the decoders run: the region goes back before the panel below is drawn.
    const LentCoverScratch scratch(*this, renderer);
    metadata = loadNextBookMetadata(nextBookPath_, scratch.arena());
  }
  // The panel places the text beside the cover, so it needs the cover's size on every layout pass.
  // Read the header once, here, rather than open the file on the render task each pass.
  std::string coverPath;
  int coverWidth = 0;
  int coverHeight = 0;
  if (!metadata.coverPath.empty()) {
    coverPath = UITheme::getCoverThumbPath(metadata.coverPath, kFinishedBookCoverMaxWidth, kFinishedBookCoverHeight);
    HalFile coverFile = Storage.open(coverPath.c_str());
    if (coverFile) {
      Bitmap bmp(coverFile);
      if (bmp.parseHeaders() == BmpReaderError::Ok && bmp.getWidth() > 0 && bmp.getHeight() > 0) {
        coverWidth = bmp.getWidth();
        coverHeight = bmp.getHeight();
      }
      coverFile.close();
    }
  }
  {
    // The render task reads all of these: the next-book row's label and the panel.
    RenderLock lock(*this);
    nextBookTitle_ = metadata.title.empty() ? getFilename(nextBookPath_) : metadata.title;
    nextBookAuthor_ = metadata.author;
    nextBookSeries_ = metadata.series;
    nextBookCoverPath_ = coverWidth > 0 ? std::move(coverPath) : std::string();
    coverWidth_ = coverWidth;
    coverHeight_ = coverHeight;
    nextBookMetadataLoaded_ = true;
  }
  requestUpdate();
}

// Runs on every layout pass (up to nine a frame): text and placement only, no SD access. The cover
// itself is drawn once, in afterUiRender().
void FinishedBookActivity::drawChrome() {
  UiListActivity::drawChrome();  // the header

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect content = listContentRect();
  const Rect header = listHeaderRect();
  const int textX = content.x + metrics.contentSidePadding;
  const int lineGap = renderer.getLineHeight(UI_10_FONT_ID);

  int y = header.y + header.height + metrics.verticalSpacing;
  renderer.drawText(UI_10_FONT_ID, textX, y, tr(STR_FINISHED_BOOK_HEADER_LINE1), true, EpdFontFamily::REGULAR);
  y += lineGap + 4;
  renderer.drawText(UI_10_FONT_ID, textX, y, tr(STR_FINISHED_BOOK_HEADER_LINE2), true, EpdFontFamily::REGULAR);
  y += lineGap + 4;

  coverW_ = 0;
  coverH_ = 0;
  if (nextBookAvailable_) {
    renderer.drawText(UI_12_FONT_ID, textX, y, tr(STR_NEXT_BOOK_HEADER), true, EpdFontFamily::BOLD);
    y += lineGap + metrics.verticalSpacing;
    y = layoutNextBookPreview(content, y) + metrics.verticalSpacing;
  }
  listTop_ = y;
}

// The next book's panel: the cover on the left, title / author / series wrapped beside it. Returns
// the panel's bottom edge. The cover is only placed here.
int FinishedBookActivity::layoutNextBookPreview(const Rect& content, const int top) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int left = content.x + metrics.contentSidePadding;
  const int contentBottom = content.y + content.height;
  const int contentWidth = content.width - 2 * metrics.contentSidePadding;
  // Leave room under the panel for about three rows of the list.
  const int previewHeight = std::max(
      0, std::min(kFinishedBookCoverHeight,
                  contentBottom - top - 3 * (renderer.getLineHeight(UI_12_FONT_ID) + 8) - metrics.verticalSpacing));
  const int previewWidth = std::min(contentWidth / 2, kFinishedBookCoverMaxWidth);

  int textX = left;
  if (coverWidth_ > 0 && coverHeight_ > 0 && previewHeight > 0) {
    int height = previewHeight;
    int width = coverWidth_ * height / coverHeight_;
    if (width > previewWidth) {
      width = previewWidth;
      height = coverHeight_ * width / coverWidth_;
    }
    if (width > 0 && height > 0) {
      coverX_ = left;
      coverY_ = top;
      coverW_ = width;
      coverH_ = height;
      textX = left + width + metrics.contentSidePadding;
    }
  }

  const int textWidth = content.x + content.width - metrics.contentSidePadding - textX;
  int infoY = top;
  if (!nextBookTitle_.empty()) {
    for (const auto& line : renderer.wrappedText(UI_12_FONT_ID, nextBookTitle_.c_str(), textWidth, 3)) {
      renderer.drawText(UI_12_FONT_ID, textX, infoY, line.c_str(), true, EpdFontFamily::BOLD);
      infoY += renderer.getLineHeight(UI_12_FONT_ID);
    }
    infoY += 4;
  }
  if (!nextBookAuthor_.empty()) {
    for (const auto& line : renderer.wrappedText(UI_10_FONT_ID, nextBookAuthor_.c_str(), textWidth, 3)) {
      renderer.drawText(UI_10_FONT_ID, textX, infoY, line.c_str(), true);
      infoY += renderer.getLineHeight(UI_10_FONT_ID);
    }
    infoY += 4;
  }
  if (!nextBookSeries_.empty()) {
    for (const auto& line : renderer.wrappedText(UI_10_FONT_ID, nextBookSeries_.c_str(), textWidth, 2)) {
      renderer.drawText(UI_10_FONT_ID, textX, infoY, line.c_str(), true);
      infoY += renderer.getLineHeight(UI_10_FONT_ID);
    }
  }
  return std::max(top + previewHeight, infoY);
}

void FinishedBookActivity::buildScreen(UiScreen& screen) {
  const Rect content = listContentRect();
  // The list starts under the panel drawChrome() laid out on this same pass, and no spacer follows
  // the margin, so layoutListArea() does not apply.
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(listTop_), static_cast<int16_t>(renderer.getScreenWidth() - (content.x + content.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (content.y + content.height)),
      static_cast<int16_t>(content.x)});

  fui::ListProps props = listProps(screen);
  props.count = actionCount_;
  props.rowProvider = &FinishedBookActivity::provideRow;
  props.rowProviderCtx = this;
  // One line per row, as before: up to six rows share the screen with the preview panel, and what a
  // second line would carry (the next book's author and series) is already in the panel.
  props.labelText.maxLines = 1;
  addList(screen, props);
}

// Once per frame, after the layout passes: drawChrome() placed the cover, this draws it.
void FinishedBookActivity::afterUiRender() {
  if (coverW_ <= 0 || coverH_ <= 0) return;
  HalFile coverFile = Storage.open(nextBookCoverPath_.c_str());
  if (!coverFile) return;
  Bitmap bmp(coverFile);
  if (bmp.parseHeaders() == BmpReaderError::Ok) renderer.drawBitmap(bmp, coverX_, coverY_, coverW_, coverH_);
  coverFile.close();
}
