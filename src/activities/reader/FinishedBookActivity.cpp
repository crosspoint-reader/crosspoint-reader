#include "FinishedBookActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <PngToBmpConverter.h>
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

#include "ReaderActivity.h"
#include "RecentBooksStore.h"
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

static constexpr int kFinishedBookCoverHeight = 240;
static constexpr int kFinishedBookCoverMaxWidth = 220;

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
                                const std::string& fileName) {
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
    ok = JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(src, dst, width, height);
  } else if (FsHelpers::hasPngExtension(sidecarPath)) {
    ok = PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(src, dst, width, height);
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

std::string getSidecarCoverBmpPath(const std::string& bookPath, int width, int height) {
  const std::string sidecarPath = ReaderActivity::sidecarCoverPath(bookPath);
  if (sidecarPath.empty()) {
    return "";
  }

  if (FsHelpers::hasBmpExtension(sidecarPath)) {
    return sidecarPath;
  }

  const std::string fileName = "thumb_" + std::to_string(width) + "x" + std::to_string(height) + ".bmp";
  return convertSidecarToBmp(ReaderActivity::bookCacheDir(bookPath), sidecarPath, width, height, fileName);
}

bool moveSidecarFilesToCompleted(const std::string& currentBookPath, const std::string& targetBookPath) {
  const auto srcDot = currentBookPath.rfind('.');
  const auto dstDot = targetBookPath.rfind('.');
  if (srcDot == std::string::npos || dstDot == std::string::npos) {
    return false;
  }

  const std::string srcBase = currentBookPath.substr(0, srcDot);
  const std::string dstBase = targetBookPath.substr(0, dstDot);
  const char* extensions[] = {".bmp", ".jpg", ".jpeg", ".png", ".BMP", ".JPG", ".JPEG", ".PNG"};
  bool success = true;
  for (const char* ext : extensions) {
    const std::string srcSidecar = srcBase + ext;
    if (!Storage.exists(srcSidecar.c_str())) {
      continue;
    }

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

NextBookMetadata loadNextBookMetadata(const std::string& nextBookPath) {
  NextBookMetadata metadata;
  if (nextBookPath.empty()) {
    return metadata;
  }

  if (FsHelpers::hasEpubExtension(nextBookPath)) {
    Epub epub(nextBookPath, "/.crosspoint");
    epub.setSyntheticTocFallbackEnabled(SETTINGS.syntheticTocFallback != 0);
    if (epub.load(false, true)) {
      metadata.title = epub.getTitle();
      metadata.author = epub.getAuthor();
      metadata.series = epub.getSeries();
      if (!metadata.series.empty() && !epub.getSeriesIndex().empty()) {
        metadata.series += " #" + epub.getSeriesIndex();
      }
      if (epub.generateThumbBmp(kFinishedBookCoverHeight)) {
        metadata.coverPath = epub.getThumbBmpPath(kFinishedBookCoverHeight);
      } else {
        metadata.coverPath = getSidecarCoverBmpPath(nextBookPath, kFinishedBookCoverMaxWidth, kFinishedBookCoverHeight);
      }
    }
    return metadata;
  }

  if (FsHelpers::hasXtcExtension(nextBookPath)) {
    Xtc xtc(nextBookPath, "/.crosspoint");
    if (xtc.load()) {
      metadata.title = xtc.getTitle();
      metadata.author = xtc.getAuthor();
      if (xtc.generateThumbBmp(kFinishedBookCoverHeight)) {
        metadata.coverPath = xtc.getThumbBmpPath(kFinishedBookCoverHeight);
      } else {
        metadata.coverPath = getSidecarCoverBmpPath(nextBookPath, kFinishedBookCoverMaxWidth, kFinishedBookCoverHeight);
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
        metadata.coverPath = getSidecarCoverBmpPath(nextBookPath, kFinishedBookCoverMaxWidth, kFinishedBookCoverHeight);
      }
    }
    return metadata;
  }

  return metadata;
}

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

std::string findSeriesSequel(const std::string& directory, const std::string& currentSeries,
                             const std::string& currentSeriesIndex) {
  const auto currentIndex = parseSeriesIndex(currentSeriesIndex);
  if (!currentIndex.has_value() || currentSeries.empty()) {
    return {};
  }

  std::vector<std::string> files;
  auto root = Storage.open(directory.c_str());
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return {};
  }

  root.rewindDirectory();
  char name[512];
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    if (file.isDirectory()) {
      file.close();
      continue;
    }
    std::string fileName = name;
    if (!FsHelpers::hasEpubExtension(fileName)) {
      file.close();
      continue;
    }
    if (fileName.empty() || fileName[0] == '.') {
      file.close();
      continue;
    }
    files.push_back(fileName);
    file.close();
  }
  root.close();
  if (files.empty()) return {};
  std::sort(files.begin(), files.end(), naturalLess);

  std::string bestPath;
  float bestIndex = std::numeric_limits<float>::infinity();
  for (const auto& fileName : files) {
    const std::string candidatePath = pathWithFilename(directory, fileName);
    auto epub = std::unique_ptr<Epub>(new Epub(candidatePath, "/.crosspoint"));
    epub->setSyntheticTocFallbackEnabled(SETTINGS.syntheticTocFallback != 0);
    if (!epub->load(true, SETTINGS.embeddedStyle == 0)) {
      continue;
    }
    const std::string candidateSeries = epub->getSeries();
    const std::string candidateSeriesIndex = epub->getSeriesIndex();
    if (!caseInsensitiveEqual(candidateSeries, currentSeries)) {
      continue;
    }
    const auto candidateIndex = parseSeriesIndex(candidateSeriesIndex);
    if (!candidateIndex.has_value()) {
      continue;
    }
    if (candidateIndex.value() <= currentIndex.value()) {
      continue;
    }
    if (candidateIndex.value() < bestIndex) {
      bestIndex = candidateIndex.value();
      bestPath = candidatePath;
    }
  }
  return bestPath;
}

std::string findNextAlphabeticalBook(const std::string& directory, const std::string& currentFilename) {
  std::vector<std::string> files;
  auto root = Storage.open(directory.c_str());
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return {};
  }

  root.rewindDirectory();
  char name[512];
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    if (file.isDirectory()) {
      file.close();
      continue;
    }
    std::string fileName = name;
    if (fileName.empty() || fileName[0] == '.' || !isSupportedBookFile(fileName)) {
      file.close();
      continue;
    }
    files.push_back(fileName);
    file.close();
  }
  root.close();
  if (files.empty()) return {};
  std::sort(files.begin(), files.end(), naturalLess);

  for (size_t i = 0; i < files.size(); ++i) {
    if (files[i] == currentFilename && i + 1 < files.size()) {
      return pathWithFilename(directory, files[i + 1]);
    }
  }
  return {};
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
    const std::string sequel = findSeriesSequel(directory, currentBookSeries, currentBookSeriesIndex);
    if (!sequel.empty() && sequel != currentBookPath) {
      return sequel;
    }
  }

  return findNextAlphabeticalBook(directory, currentFilename);
}

bool moveFinishedBookToCompleted(const std::string& currentBookPath, std::string& outMovedPath) {
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

  outMovedPath = targetPath;
  return true;
}

}  // namespace BookFinished

FinishedBookActivity::FinishedBookActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           std::string currentBookPath, std::string nextBookPath)
    : Activity("FinishedBook", renderer, mappedInput),
      currentBookPath_(std::move(currentBookPath)),
      nextBookPath_(std::move(nextBookPath)),
      nextBookAvailable_(!nextBookPath_.empty()) {
  nextBookName_ = nextBookAvailable_ ? getFilename(nextBookPath_) : tr(STR_NOT_SET);
}

void FinishedBookActivity::onEnter() {
  Activity::onEnter();
  const bool canMoveToCompleted = !pathIsInCompleted(currentBookPath_);
  const int optionCount = 1 + (nextBookAvailable_ ? 1 : 0) + (canMoveToCompleted ? 1 : 0) + 1;
  selectedIndex_ = std::clamp(selectedIndex_, 0, optionCount - 1);
  moveFinishedBooksToCompleted_ = SETTINGS.moveFinishedBooksToCompleted;
  removeFinishedBooksFromRecents_ = SETTINGS.removeFinishedBooksFromRecents;

  if (nextBookAvailable_) {
    nextBookTitle_ = nextBookName_;
    nextBookAuthor_.clear();
    nextBookSeries_.clear();
    nextBookCoverPath_.clear();
    nextBookMetadataLoaded_ = false;
  } else {
    nextBookTitle_.clear();
    nextBookAuthor_.clear();
    nextBookSeries_.clear();
    nextBookCoverPath_.clear();
    nextBookMetadataLoaded_ = true;
  }
  requestUpdate();
}

void FinishedBookActivity::loop() {
  ButtonEventManager::ButtonEvent ev;
  while (buttonEvents.consumeEvent(ev)) {
    if (ev.type != ButtonEventManager::PressType::Short) {
      continue;
    }

    const bool canMoveToCompleted = !pathIsInCompleted(currentBookPath_);
    const int optionCount = 1 + (nextBookAvailable_ ? 1 : 0) + (canMoveToCompleted ? 1 : 0) + 1;

    if (ev.button == MappedInputManager::Button::Back) {
      MenuResult menuResult;
      menuResult.action = static_cast<int>(BookFinished::FinishedBookAction::GoHome);
      ActivityResult result(menuResult);
      setResult(std::move(result));
      finish();
      return;
    }

    if (ev.button == MappedInputManager::Button::Confirm) {
      if (selectedIndex_ == 0) {
        MenuResult menuResult;
        menuResult.action = static_cast<int>(BookFinished::FinishedBookAction::GoHome);
        ActivityResult result(menuResult);
        setResult(std::move(result));
        finish();
        return;
      }

      if (selectedIndex_ == 1 && nextBookAvailable_) {
        MenuResult menuResult;
        menuResult.action = static_cast<int>(BookFinished::FinishedBookAction::OpenNextBook);
        ActivityResult result(menuResult);
        setResult(std::move(result));
        finish();
        return;
      }

      const int moveIndex = 1 + (nextBookAvailable_ ? 1 : 0);
      if (selectedIndex_ == moveIndex && canMoveToCompleted) {
        moveFinishedBooksToCompleted_ = !moveFinishedBooksToCompleted_;
        SETTINGS.moveFinishedBooksToCompleted = moveFinishedBooksToCompleted_;
        SETTINGS.saveToFile();
        requestUpdate();
        return;
      }

      const int removeIndex = 1 + (nextBookAvailable_ ? 1 : 0) + (canMoveToCompleted ? 1 : 0);
      if (selectedIndex_ == removeIndex) {
        removeFinishedBooksFromRecents_ = !removeFinishedBooksFromRecents_;
        SETTINGS.removeFinishedBooksFromRecents = removeFinishedBooksFromRecents_;
        SETTINGS.saveToFile();
        requestUpdate();
        return;
      }
      return;
    }

    if (ev.button == MappedInputManager::Button::Down || ev.button == MappedInputManager::Button::Right) {
      selectedIndex_ = (selectedIndex_ + 1) % optionCount;
      requestUpdate();
      return;
    }

    if (ev.button == MappedInputManager::Button::Up || ev.button == MappedInputManager::Button::Left) {
      selectedIndex_ = (selectedIndex_ + optionCount - 1) % optionCount;
      requestUpdate();
      return;
    }
  }

  if (nextBookAvailable_ && !nextBookMetadataLoaded_) {
    const auto metadata = loadNextBookMetadata(nextBookPath_);
    nextBookTitle_ = metadata.title.empty() ? getFilename(nextBookPath_) : metadata.title;
    nextBookAuthor_ = metadata.author;
    nextBookSeries_ = metadata.series;
    nextBookCoverPath_ = metadata.coverPath;
    nextBookMetadataLoaded_ = true;
    requestUpdate();
  }
}

void FinishedBookActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);
  const int contentTop = contentRect.y;
  const int contentBottom = contentRect.y + contentRect.height - metrics.buttonHintsHeight;
  const int contentWidth = contentRect.width - 2 * metrics.contentSidePadding;
  int y = contentTop;
  int yGap = renderer.getLineHeight(UI_10_FONT_ID);
  GUI.drawHeader(renderer, Rect{contentRect.x, contentTop, contentRect.width, metrics.headerHeight},
                 tr(STR_FINISHED_BOOK_HEADER), "");
  y += metrics.headerHeight + metrics.verticalSpacing;
  renderer.drawText(UI_10_FONT_ID, contentRect.x + metrics.contentSidePadding, y, tr(STR_FINISHED_BOOK_HEADER_LINE1),
                    true, EpdFontFamily::REGULAR);
  y += yGap + 4;
  renderer.drawText(UI_10_FONT_ID, contentRect.x + metrics.contentSidePadding, y, tr(STR_FINISHED_BOOK_HEADER_LINE2),
                    true, EpdFontFamily::REGULAR);
  y += yGap + 4;

  if (nextBookAvailable_) {
    renderer.drawText(UI_12_FONT_ID, contentRect.x + metrics.contentSidePadding, y, tr(STR_NEXT_BOOK_HEADER), true,
                      EpdFontFamily::BOLD);
    y += yGap + metrics.verticalSpacing;
  }

  const int previewHeight =
      nextBookAvailable_ ? std::max(0, std::min(kFinishedBookCoverHeight,
                                                contentBottom - y - 3 * (renderer.getLineHeight(UI_12_FONT_ID) + 8) -
                                                    metrics.verticalSpacing))
                         : 0;
  const int previewWidth = nextBookAvailable_ ? std::min(contentWidth / 2, kFinishedBookCoverMaxWidth) : 0;
  const int previewX = contentRect.x + metrics.contentSidePadding;
  const int previewY = y;

  if (nextBookAvailable_) {
    int actualCoverWidth = 0;
    int previewTextX = previewX;

    if (!nextBookCoverPath_.empty()) {
      const std::string coverPath = UITheme::getCoverThumbPath(nextBookCoverPath_, previewWidth, previewHeight);
      HalFile coverFile = Storage.open(coverPath.c_str());
      if (coverFile) {
        Bitmap bmp(coverFile);
        if (bmp.parseHeaders() == BmpReaderError::Ok && bmp.getWidth() > 0 && bmp.getHeight() > 0) {
          int actualCoverHeight = previewHeight;
          actualCoverWidth = bmp.getWidth() * actualCoverHeight / bmp.getHeight();
          if (actualCoverWidth > previewWidth) {
            actualCoverWidth = previewWidth;
            actualCoverHeight = bmp.getHeight() * actualCoverWidth / bmp.getWidth();
          }
          renderer.drawBitmap(bmp, previewX, previewY, actualCoverWidth, actualCoverHeight);
        }
        coverFile.close();
      }
    }

    if (actualCoverWidth > 0) {
      previewTextX = previewX + actualCoverWidth + metrics.contentSidePadding;
    }

    const int previewTextWidth = contentRect.x + contentRect.width - metrics.contentSidePadding - previewTextX;
    int infoY = previewY;
    const auto titleLines = renderer.wrappedText(UI_12_FONT_ID, nextBookTitle_.c_str(), previewTextWidth, 3);
    const auto authorLines = renderer.wrappedText(UI_10_FONT_ID, nextBookAuthor_.c_str(), previewTextWidth, 3);
    const auto seriesLines = renderer.wrappedText(UI_10_FONT_ID, nextBookSeries_.c_str(), previewTextWidth, 2);

    if (!nextBookTitle_.empty()) {
      for (const auto& line : titleLines) {
        renderer.drawText(UI_12_FONT_ID, previewTextX, infoY, line.c_str(), true, EpdFontFamily::BOLD);
        infoY += renderer.getLineHeight(UI_12_FONT_ID);
      }
      infoY += 4;
    }
    if (!nextBookAuthor_.empty()) {
      for (const auto& line : authorLines) {
        renderer.drawText(UI_10_FONT_ID, previewTextX, infoY, line.c_str(), true);
        infoY += renderer.getLineHeight(UI_10_FONT_ID);
      }
      infoY += 4;
    }
    if (!nextBookSeries_.empty()) {
      for (const auto& line : seriesLines) {
        renderer.drawText(UI_10_FONT_ID, previewTextX, infoY, line.c_str(), true);
        infoY += renderer.getLineHeight(UI_10_FONT_ID);
      }
    }
    y = std::max(y + previewHeight, infoY) + metrics.verticalSpacing;
  }

  const bool currentBookIsCompleted = pathIsInCompleted(currentBookPath_);
  const bool canMoveToCompleted = !currentBookIsCompleted;
  const int optionCount = 1 + (nextBookAvailable_ ? 1 : 0) + (canMoveToCompleted ? 1 : 0) + 1;

  std::vector<std::string> rowTitles;
  std::vector<std::string> rowSubtitles;
  std::vector<std::string> rowValues;

  rowTitles.push_back(tr(STR_GO_BACK_TO_HOME));
  rowSubtitles.push_back(tr(STR_GO_BACK_TO_HOME_DESC));
  rowValues.push_back(tr(STR_HOME));

  if (nextBookAvailable_) {
    std::string subtitle;
    if (!nextBookAuthor_.empty()) {
      subtitle = nextBookAuthor_;
    }
    if (!nextBookSeries_.empty()) {
      if (!subtitle.empty()) subtitle += " • ";
      subtitle += nextBookSeries_;
    }
    if (subtitle.empty()) {
      subtitle = nextBookName_;
    }
    rowTitles.push_back(nextBookTitle_.empty() ? tr(STR_OPEN_NEXT_BOOK) : nextBookTitle_);
    rowSubtitles.push_back(subtitle);
    rowValues.push_back(tr(STR_OPEN));
  }

  if (canMoveToCompleted) {
    rowTitles.push_back(tr(STR_MOVE_FINISHED_TO_COMPLETED));
    rowSubtitles.push_back("");
    rowValues.push_back(moveFinishedBooksToCompleted_ ? tr(STR_STATE_ON) : tr(STR_STATE_OFF));
  }

  rowTitles.push_back(tr(STR_FORGET_BOOK));
  rowSubtitles.push_back(tr(STR_FORGET_BOOK_DESC));
  rowValues.push_back(removeFinishedBooksFromRecents_ ? tr(STR_STATE_ON) : tr(STR_STATE_OFF));

  const Rect listRect{contentRect.x, y, contentRect.width, contentBottom - y};
  GUI.drawList(
      renderer, listRect, optionCount, selectedIndex_, [&rowTitles](int index) { return rowTitles[index]; },
      [&rowSubtitles](int index) { return rowSubtitles[index]; }, nullptr,
      [&rowValues](int index) { return rowValues[index]; }, true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
