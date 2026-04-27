#include "SleepActivity.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Serialization.h>
#include <Txt.h>
#include <Xtc.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/Logo120.h"

void SleepActivity::onEnter() {
  Activity::onEnter();

  // Show popup with reader orientation only when going to sleep from reader
  if (APP_STATE.lastSleepFromReader) {
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
    GUI.drawPopup(renderer, tr(STR_ENTERING_SLEEP));
    renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  } else {
    GUI.drawPopup(renderer, tr(STR_ENTERING_SLEEP));
  }

  switch (SETTINGS.sleepScreen) {
    case (CrossPointSettings::SLEEP_SCREEN_MODE::BLANK):
      return renderBlankSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM):
      return renderCustomSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER):
      return renderCoverSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM):
      if (APP_STATE.lastSleepFromReader) {
        return renderCoverSleepScreen();
      } else {
        return renderCustomSleepScreen();
      }
    default:
      return renderDefaultSleepScreen();
  }
}

void SleepActivity::renderCustomSleepScreen() const {
  // Check if we have a /.sleep (preferred) or /sleep directory
  const char* sleepDir = nullptr;
  auto dir = Storage.open("/.sleep");
  if (dir && dir.isDirectory()) {
    sleepDir = "/.sleep";
  } else {
    dir = Storage.open("/sleep");
    if (dir && dir.isDirectory()) {
      sleepDir = "/sleep";
    }
  }

  if (sleepDir) {
    std::vector<std::string> files;
    char name[500];
    // collect all valid BMP files
    for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
      if (file.isDirectory()) {
        continue;
      }
      file.getName(name, sizeof(name));
      auto filename = std::string(name);
      if (filename[0] == '.') {
        continue;
      }

      if (!FsHelpers::hasBmpExtension(filename)) {
        LOG_DBG("SLP", "Skipping non-.bmp file name: %s", name);
        continue;
      }
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() != BmpReaderError::Ok) {
        LOG_DBG("SLP", "Skipping invalid BMP file: %s", name);
        continue;
      }
      files.emplace_back(filename);
    }
    const auto numFiles = files.size();
    if (numFiles > 0) {
      // Pick a random wallpaper, excluding recently shown ones.
      // Window: up to SLEEP_RECENT_COUNT entries, capped at numFiles-1.
      const uint16_t fileCount = static_cast<uint16_t>(std::min(numFiles, static_cast<size_t>(UINT16_MAX)));
      const uint8_t window =
          static_cast<uint8_t>(std::min(static_cast<size_t>(APP_STATE.recentSleepFill), numFiles - 1));
      auto randomFileIndex = static_cast<uint16_t>(random(fileCount));
      for (uint8_t attempt = 0; attempt < 20 && APP_STATE.isRecentSleep(randomFileIndex, window); attempt++) {
        randomFileIndex = static_cast<uint16_t>(random(fileCount));
      }
      APP_STATE.pushRecentSleep(randomFileIndex);
      APP_STATE.saveToFile();
      const auto filename = std::string(sleepDir) + "/" + files[randomFileIndex];
      FsFile file;
      if (Storage.openFileForRead("SLP", filename, file)) {
        LOG_DBG("SLP", "Randomly loading: %s/%s", sleepDir, files[randomFileIndex].c_str());
        delay(100);
        Bitmap bitmap(file, true);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          renderBitmapSleepScreen(bitmap, BookOverlayInfo{});
          file.close();
          dir.close();
          return;
        }
      }
    }
  }
  // Look for sleep.bmp on the root of the sd card to determine if we should
  // render a custom sleep screen instead of the default.
  FsFile file;
  if (Storage.openFileForRead("SLP", "/sleep.bmp", file)) {
    Bitmap bitmap(file, true);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      LOG_DBG("SLP", "Loading: /sleep.bmp");
      renderBitmapSleepScreen(bitmap, BookOverlayInfo{});
      file.close();
      return;
    }
  }

  renderDefaultSleepScreen();
}

void SleepActivity::renderDefaultSleepScreen() const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  renderer.drawImage(Logo120, (pageWidth - 120) / 2, (pageHeight - 120) / 2, 120, 120);
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 70, tr(STR_CROSSPOINT), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 95, tr(STR_SLEEPING));

  // Make sleep screen dark unless light is selected in settings
  if (SETTINGS.sleepScreen != CrossPointSettings::SLEEP_SCREEN_MODE::LIGHT) {
    renderer.invertScreen();
  }

  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

BookOverlayInfo SleepActivity::getBookOverlayInfo(const Xtc& xtc) const {
  BookOverlayInfo info;
  info.title = xtc.getTitle();
  info.author = xtc.getAuthor();

  FsFile f;
  if (Storage.openFileForRead("SLP", xtc.getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      uint32_t currentPage = static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
                             (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
      uint32_t totalPages = xtc.getPageCount();
      float progress = static_cast<float>(xtc.calculateProgress(currentPage));
      char buf[64];
      snprintf(buf, sizeof(buf), tr(STR_OVERLAY_READING_PROGRESS), (unsigned long)currentPage + 1, totalPages,
               progress);
      info.progressText = buf;
    }
    f.close();
  }

  return info;
}

BookOverlayInfo SleepActivity::getBookOverlayInfo(const Txt& txt) const {
  BookOverlayInfo info;
  info.title = txt.getTitle();

  FsFile f;
  if (Storage.openFileForRead("SLP", txt.getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      uint32_t currentPage = data[0] + (data[1] << 8);

      uint32_t totalPages = 0;
      FsFile indexFile;
      if (Storage.openFileForRead("SLP", txt.getCachePath() + "/index.bin", indexFile)) {
        uint32_t magic;
        serialization::readPod(indexFile, magic);
        uint8_t version;
        serialization::readPod(indexFile, version);
        static constexpr uint32_t INDEX_CACHE_MAGIC = 0x54585449;  // "TXTI"
        static constexpr uint8_t INDEX_CACHE_VERSION = 2;
        if (magic == INDEX_CACHE_MAGIC && version == INDEX_CACHE_VERSION) {
          indexFile.seek(26);
          serialization::readPod(indexFile, totalPages);
        }
        indexFile.close();
      }

      if (totalPages > 0) {
        float progress = (currentPage + 1) * 100.0f / totalPages;
        char buf[64];
        snprintf(buf, sizeof(buf), tr(STR_OVERLAY_READING_PROGRESS), (unsigned long)currentPage + 1, totalPages,
                 progress);
        info.progressText = buf;
      } else {
        char buf[64];
        snprintf(buf, sizeof(buf), tr(STR_OVERLAY_READING_PROGRESS_NO_TOTAL), (unsigned long)currentPage + 1);
        info.progressText = buf;
      }
    }
    f.close();
  }

  return info;
}

BookOverlayInfo SleepActivity::getBookOverlayInfo(const Epub& epub) const {
  BookOverlayInfo info;
  info.title = epub.getTitle();
  info.author = epub.getAuthor();

  FsFile f;
  if (Storage.openFileForRead("SLP", epub.getCachePath() + "/progress.bin", f)) {
    uint8_t data[6];
    int dataSize = f.read(data, 6);
    if (dataSize >= 4) {
      int currentSpineIndex = data[0] + (data[1] << 8);
      int currentPage = data[2] + (data[3] << 8);
      int pageCount = (dataSize == 6) ? (data[4] + (data[5] << 8)) : 0;

      const int tocIndex = epub.getTocIndexForSpineIndex(currentSpineIndex);
      if (tocIndex != -1) {
        const auto tocItem = epub.getTocItem(tocIndex);
        info.chapterName = tocItem.title;
        if (pageCount > 0) {
          float chapterProgress = static_cast<float>(currentPage + 1) / static_cast<float>(pageCount);
          float bookProgress = epub.calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
          char suffix[64];
          snprintf(suffix, sizeof(suffix), tr(STR_OVERLAY_CHAPTER_PAGE_SUFFIX), currentPage + 1, pageCount,
                   bookProgress);
          info.progressSuffix = suffix;
        }
        info.progressText = std::string(tr(STR_CHAPTER_PREFIX)) + info.chapterName + info.progressSuffix;
      } else if (pageCount > 0) {
        float chapterProgress = static_cast<float>(currentPage + 1) / static_cast<float>(pageCount);
        float bookProgress = epub.calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
        char buf[80];
        snprintf(buf, sizeof(buf), tr(STR_OVERLAY_READING_PROGRESS), (unsigned long)currentPage + 1,
                 (unsigned)pageCount, bookProgress);
        info.progressText = buf;
      }
    }
    f.close();
  }

  return info;
}

void SleepActivity::renderBitmapSleepScreen(const Bitmap& bitmap, const BookOverlayInfo& overlayInfo) const {
  int x, y;
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  float cropX = 0, cropY = 0;
  const bool overlayActive =
      !overlayInfo.title.empty() || !overlayInfo.author.empty() || !overlayInfo.progressText.empty();

  LOG_DBG("SLP", "bitmap %d x %d, screen %d x %d", bitmap.getWidth(), bitmap.getHeight(), pageWidth, pageHeight);
  if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
    // image will scale, make sure placement is right
    float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
    const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

    LOG_DBG("SLP", "bitmap ratio: %f, screen ratio: %f", ratio, screenRatio);
    if (ratio > screenRatio) {
      // image wider than viewport ratio, scaled down image needs to be centered vertically
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        cropX = 1.0f - (screenRatio / ratio);
        LOG_DBG("SLP", "Cropping bitmap x: %f", cropX);
        ratio = (1.0f - cropX) * static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
      }
      x = 0;
      y = overlayActive ? 0 : std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
      LOG_DBG("SLP", "%s with ratio %f to y=%d", overlayActive ? "Top-aligning" : "Centering", ratio, y);
    } else {
      // image taller than viewport ratio, scaled down image needs to be centered horizontally
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        cropY = 1.0f - (ratio / screenRatio);
        LOG_DBG("SLP", "Cropping bitmap y: %f", cropY);
        ratio = static_cast<float>(bitmap.getWidth()) / ((1.0f - cropY) * static_cast<float>(bitmap.getHeight()));
      }
      x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
      y = 0;
      LOG_DBG("SLP", "Centering with ratio %f to x=%d", ratio, x);
    }
  } else {
    // center the image horizontally, vertically center or top-align depending on overlay
    x = (pageWidth - bitmap.getWidth()) / 2;
    y = overlayActive ? 0 : (pageHeight - bitmap.getHeight()) / 2;
  }

  LOG_DBG("SLP", "drawing to %d x %d", x, y);
  renderer.clearScreen();

  const bool hasGreyscale = bitmap.hasGreyscale() &&
                            SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER;

  renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);

  if (SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE) {
    renderer.invertScreen();
  }

  const uint8_t overlayMode = SETTINGS.sleepCoverOverlay;
  const auto drawOverlay = [&]() {
    const bool hasTitle = !overlayInfo.title.empty();
    const bool hasProgress = !overlayInfo.progressText.empty();
    const bool hasAuthor = !overlayInfo.author.empty();
    if (!hasTitle && !hasAuthor && !hasProgress) {
      return;
    }

    const int lineHeight12 = renderer.getLineHeight(NOTOSERIF_12_FONT_ID);
    const int lineHeight10 = renderer.getLineHeight(UI_10_FONT_ID);
    constexpr int lineSpacing = 3;
    constexpr int sectionSpacing = 10;
    const int availableWidth = pageWidth - 20;

    int textBlockHeight = 0;
    if (hasTitle) {
      textBlockHeight += lineHeight12;
      if (hasAuthor) {
        textBlockHeight += lineSpacing;
      } else if (hasProgress) {
        textBlockHeight += sectionSpacing;
      }
    }
    if (hasAuthor) {
      textBlockHeight += lineHeight10;
      if (hasProgress) {
        textBlockHeight += sectionSpacing;
      }
    }
    if (hasProgress) {
      textBlockHeight += lineHeight10;
    }

    const bool textBlack = (overlayMode != CrossPointSettings::OVERLAY_BLACK);
    const int topPadding = lineHeight12 / 3;
    const int bottomPadding = lineHeight10 * 2 / 3;
    const int overlayHeight = textBlockHeight + topPadding + bottomPadding;
    const int overlayY = pageHeight - overlayHeight;

    if (overlayMode == CrossPointSettings::OVERLAY_GRAY) {
      renderer.fillRectDither(0, overlayY, pageWidth, overlayHeight, Color::LightGray);
    } else {
      renderer.fillRect(0, overlayY, pageWidth, overlayHeight, overlayMode == CrossPointSettings::OVERLAY_BLACK);
    }

    int currentY = overlayY + topPadding;

    if (hasTitle) {
      const std::string titleStr =
          renderer.truncatedText(NOTOSERIF_12_FONT_ID, overlayInfo.title.c_str(), availableWidth, EpdFontFamily::BOLD);
      renderer.drawCenteredText(NOTOSERIF_12_FONT_ID, currentY, titleStr.c_str(), textBlack, EpdFontFamily::BOLD);
      const int spacingAfterTitle = hasAuthor ? lineSpacing : (hasProgress ? sectionSpacing : lineSpacing);
      currentY += lineHeight12 + spacingAfterTitle;
    }

    if (hasAuthor) {
      const std::string authorStr = renderer.truncatedText(UI_10_FONT_ID, overlayInfo.author.c_str(), availableWidth);
      renderer.drawCenteredText(UI_10_FONT_ID, currentY, authorStr.c_str(), textBlack);
      currentY += lineHeight10 + sectionSpacing;
    }

    if (hasProgress) {
      std::string progressStr;
      if (!overlayInfo.chapterName.empty()) {
        const std::string prefix = tr(STR_CHAPTER_PREFIX);
        const int prefixWidth = renderer.getTextWidth(UI_10_FONT_ID, prefix.c_str());
        const int suffixWidth = renderer.getTextWidth(UI_10_FONT_ID, overlayInfo.progressSuffix.c_str());
        const int maxChapterWidth = availableWidth - prefixWidth - suffixWidth;
        const std::string truncatedChapter =
            maxChapterWidth > 0
                ? renderer.truncatedText(UI_10_FONT_ID, overlayInfo.chapterName.c_str(), maxChapterWidth)
                : "";
        progressStr = prefix + truncatedChapter + overlayInfo.progressSuffix;
      } else {
        progressStr = renderer.truncatedText(UI_10_FONT_ID, overlayInfo.progressText.c_str(), availableWidth);
      }
      renderer.drawCenteredText(UI_10_FONT_ID, currentY, progressStr.c_str(), textBlack);
    }
  };

  drawOverlay();

  renderer.displayBuffer(HalDisplay::HALF_REFRESH);

  if (hasGreyscale) {
    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    drawOverlay();
    renderer.copyGrayscaleLsbBuffers();

    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    drawOverlay();
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
  }
}

void SleepActivity::renderCoverSleepScreen() const {
  void (SleepActivity::*renderNoCoverSleepScreen)() const;
  switch (SETTINGS.sleepScreen) {
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM):
      renderNoCoverSleepScreen = &SleepActivity::renderCustomSleepScreen;
      break;
    default:
      renderNoCoverSleepScreen = &SleepActivity::renderDefaultSleepScreen;
      break;
  }

  if (APP_STATE.openEpubPath.empty()) {
    return (this->*renderNoCoverSleepScreen)();
  }

  std::string coverBmpPath;
  bool cropped = SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP;
  BookOverlayInfo coverOverlayInfo;

  const uint8_t overlayMode = SETTINGS.sleepCoverOverlay;
  const bool overlayEnabled = overlayMode != CrossPointSettings::OVERLAY_OFF;

  if (FsHelpers::hasXtcExtension(APP_STATE.openEpubPath)) {
    Xtc lastXtc(APP_STATE.openEpubPath, "/.crosspoint");
    if (!lastXtc.load()) {
      LOG_ERR("SLP", "Failed to load last XTC");
      return (this->*renderNoCoverSleepScreen)();
    }

    if (!lastXtc.generateCoverBmp()) {
      LOG_ERR("SLP", "Failed to generate XTC cover bmp");
      return (this->*renderNoCoverSleepScreen)();
    }

    coverBmpPath = lastXtc.getCoverBmpPath();
    if (overlayEnabled) {
      coverOverlayInfo = getBookOverlayInfo(lastXtc);
    }
  } else if (FsHelpers::hasTxtExtension(APP_STATE.openEpubPath)) {
    Txt lastTxt(APP_STATE.openEpubPath, "/.crosspoint");
    if (!lastTxt.load()) {
      LOG_ERR("SLP", "Failed to load last TXT");
      return (this->*renderNoCoverSleepScreen)();
    }

    if (!lastTxt.generateCoverBmp()) {
      LOG_ERR("SLP", "No cover image found for TXT file");
      return (this->*renderNoCoverSleepScreen)();
    }

    coverBmpPath = lastTxt.getCoverBmpPath();
    if (overlayEnabled) {
      coverOverlayInfo = getBookOverlayInfo(lastTxt);
    }
  } else if (FsHelpers::hasEpubExtension(APP_STATE.openEpubPath)) {
    Epub lastEpub(APP_STATE.openEpubPath, "/.crosspoint");
    if (!lastEpub.load(true, true)) {
      LOG_ERR("SLP", "Failed to load last epub");
      return (this->*renderNoCoverSleepScreen)();
    }

    if (!lastEpub.generateCoverBmp(cropped)) {
      LOG_ERR("SLP", "Failed to generate cover bmp");
      return (this->*renderNoCoverSleepScreen)();
    }

    coverBmpPath = lastEpub.getCoverBmpPath(cropped);
    if (overlayEnabled) {
      coverOverlayInfo = getBookOverlayInfo(lastEpub);
    }
  } else {
    return (this->*renderNoCoverSleepScreen)();
  }

  FsFile file;
  if (Storage.openFileForRead("SLP", coverBmpPath, file)) {
    Bitmap bitmap(file);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      LOG_DBG("SLP", "Rendering sleep cover: %s", coverBmpPath.c_str());
      renderBitmapSleepScreen(bitmap, coverOverlayInfo);
      file.close();
      return;
    }
  }

  return (this->*renderNoCoverSleepScreen)();
}

void SleepActivity::renderBlankSleepScreen() const {
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}
