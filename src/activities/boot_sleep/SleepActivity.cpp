#include "SleepActivity.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Txt.h>
#include <Xtc.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/Logo120.h"

void SleepActivity::onEnter() {
  Activity::onEnter();
  GUI.drawPopup(renderer, tr(STR_ENTERING_SLEEP));

  switch (SETTINGS.sleepScreen) {
    case (CrossPointSettings::SLEEP_SCREEN_MODE::BLANK):
      return renderBlankSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM):
      return renderCustomSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER):
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM):
      return renderCoverSleepScreen();
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
    if (dir) dir.close();
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
        file.close();
        continue;
      }
      file.getName(name, sizeof(name));
      auto filename = std::string(name);
      if (filename[0] == '.') {
        file.close();
        continue;
      }

      if (!FsHelpers::hasBmpExtension(filename)) {
        LOG_DBG("SLP", "Skipping non-.bmp file name: %s", name);
        file.close();
        continue;
      }
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() != BmpReaderError::Ok) {
        LOG_DBG("SLP", "Skipping invalid BMP file: %s", name);
        file.close();
        continue;
      }
      files.emplace_back(filename);
      file.close();
    }
    const auto numFiles = files.size();
    if (numFiles > 0) {
      // Generate a random number between 1 and numFiles
      auto randomFileIndex = random(numFiles);
      // If we picked the same image as last time, reroll
      while (numFiles > 1 && APP_STATE.lastSleepImage != UINT8_MAX && randomFileIndex == APP_STATE.lastSleepImage) {
        randomFileIndex = random(numFiles);
      }
      APP_STATE.lastSleepImage = randomFileIndex;
      APP_STATE.saveToFile();
      const auto filename = std::string(sleepDir) + "/" + files[randomFileIndex];
      FsFile file;
      if (Storage.openFileForRead("SLP", filename, file)) {
        LOG_DBG("SLP", "Randomly loading: %s/%s", sleepDir, files[randomFileIndex].c_str());
        delay(100);
        Bitmap bitmap(file, true);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          renderBitmapSleepScreen(bitmap);
          file.close();
          dir.close();
          return;
        }
        file.close();
      }
    }
  }
  if (dir) dir.close();

  // Look for sleep.bmp on the root of the sd card to determine if we should
  // render a custom sleep screen instead of the default.
  FsFile file;
  if (Storage.openFileForRead("SLP", "/sleep.bmp", file)) {
    Bitmap bitmap(file, true);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      LOG_DBG("SLP", "Loading: /sleep.bmp");
      renderBitmapSleepScreen(bitmap);
      file.close();
      return;
    }
    file.close();
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

BookOverlayInfo SleepActivity::getBookOverlayInfo(const std::string& bookPath) const {
  BookOverlayInfo info;

  if (FsHelpers::checkFileExtension(bookPath, ".xtc") || FsHelpers::checkFileExtension(bookPath, ".xtch")) {
    Xtc xtc(bookPath, "/.crosspoint");
    if (xtc.load()) {
      info.title = xtc.getTitle();
      info.author = xtc.getAuthor();

      FsFile f;
      if (Storage.openFileForRead("SLP", xtc.getCachePath() + "/progress.bin", f)) {
        uint8_t data[4];
        if (f.read(data, 4) == 4) {
          uint32_t currentPage = static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
                                 (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
          uint32_t totalPages = xtc.getPageCount();
          float progress = xtc.calculateProgress(currentPage) * 100.0f;
          char buf[64];
          snprintf(buf, sizeof(buf), "Reading progress: Page %lu/%u - %.0f%%", (unsigned long)currentPage + 1,
                   totalPages, progress);
          info.progressText = buf;
        }
        f.close();
      }
    }
  } else if (FsHelpers::checkFileExtension(bookPath, ".txt")) {
    Txt txt(bookPath, "/.crosspoint");
    if (txt.load()) {
      info.title = txt.getTitle();

      FsFile f;
      if (Storage.openFileForRead("SLP", txt.getCachePath() + "/progress.bin", f)) {
        uint8_t data[4];
        if (f.read(data, 4) == 4) {
          uint32_t currentPage = data[0] + (data[1] << 8);

          uint32_t totalPages = 0;
          FsFile indexFile;
          if (Storage.openFileForRead("SLP", txt.getCachePath() + "/index.bin", indexFile)) {
            indexFile.seek(32);
            if (indexFile.read(reinterpret_cast<uint8_t*>(&totalPages), sizeof(totalPages)) == sizeof(totalPages)) {
            }
            indexFile.close();
          }

          if (totalPages > 0) {
            float progress = (currentPage + 1) * 100.0f / totalPages;
            char buf[64];
            snprintf(buf, sizeof(buf), "Reading progress: Page %lu/%u - %.0f%%", (unsigned long)currentPage + 1,
                     totalPages, progress);
            info.progressText = buf;
          } else {
            char buf[64];
            snprintf(buf, sizeof(buf), "Reading progress: Page %lu", (unsigned long)currentPage + 1);
            info.progressText = buf;
          }
        }
        f.close();
      }
    }
  } else if (FsHelpers::checkFileExtension(bookPath, ".epub")) {
    Epub epub(bookPath, "/.crosspoint");
    if (epub.load(true, true)) {
      info.title = epub.getTitle();
      info.author = epub.getAuthor();

      FsFile f;
      if (Storage.openFileForRead("SLP", epub.getCachePath() + "/progress.bin", f)) {
        uint8_t data[6];
        if (f.read(data, 6) == 6) {
          int currentSpineIndex = data[0] + (data[1] << 8);
          int currentPage = data[2] + (data[3] << 8);
          int pageCount = data[4] + (data[5] << 8);
          if (pageCount > 0) {
            float chapterProgress = static_cast<float>(currentPage) / static_cast<float>(pageCount);
            float bookProgress = epub.calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;

            const int tocIndex = epub.getTocIndexForSpineIndex(currentSpineIndex);
            if (tocIndex != -1) {
              const auto tocItem = epub.getTocItem(tocIndex);
              info.chapterName = tocItem.title;
              char suffix[64];
              snprintf(suffix, sizeof(suffix), " - Page %d/%d - %.0f%%", currentPage + 1, pageCount, bookProgress);
              info.progressSuffix = suffix;
              info.progressText = "Chapter: " + info.chapterName + info.progressSuffix;
            } else {
              char buf[80];
              snprintf(buf, sizeof(buf), "Reading progress: Page %d/%d - %.0f%%", currentPage + 1, pageCount,
                       bookProgress);
              info.progressText = buf;
            }
          }
        }
        f.close();
      }
    }
  }

  return info;
}

void SleepActivity::renderBitmapSleepScreen(const Bitmap& bitmap) const {
  int x, y;
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  float cropX = 0, cropY = 0;

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
      y = 0;
      LOG_DBG("SLP", "Centering with ratio %f to y=%d", ratio, y);
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
    // center the image
    x = (pageWidth - bitmap.getWidth()) / 2;
    y = 0;
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
  const BookOverlayInfo overlayInfo = overlayMode < 3 ? getBookOverlayInfo(APP_STATE.openEpubPath) : BookOverlayInfo{};
  const auto drawOverlay = [&]() {
    const bool hasTitle = !overlayInfo.title.empty();
    const bool hasProgress = !overlayInfo.progressText.empty();
    const bool hasAuthor = !overlayInfo.author.empty();
    if (!hasTitle && !hasAuthor && !hasProgress) {
      return;
    }

    const int lineHeight12 = renderer.getLineHeight(BOOKERLY_12_FONT_ID);
    const int lineHeight10 = renderer.getLineHeight(UI_10_FONT_ID);
    constexpr int lineSpacing = 3;
    constexpr int sectionSpacing = 10;
    const int availableWidth = pageWidth - 20;

    int textBlockHeight = 0;
    if (hasTitle) textBlockHeight += lineHeight12;
    if (hasAuthor) textBlockHeight += lineSpacing + lineHeight10;
    if (hasProgress) textBlockHeight += sectionSpacing + lineHeight10;

    const bool textBlack = (overlayMode != 2);
    const int topPadding = lineHeight12 / 3;
    const int bottomPadding = lineHeight10 * 2 / 3;
    const int overlayHeight = textBlockHeight + topPadding + bottomPadding;
    const int overlayY = pageHeight - overlayHeight;

    if (overlayMode == 1) {
      renderer.fillRectDither(0, overlayY, pageWidth, overlayHeight, Color::LightGray);
    } else {
      renderer.fillRect(0, overlayY, pageWidth, overlayHeight, overlayMode == 2);
    }

    int currentY = overlayY + topPadding;

    if (hasTitle) {
      const std::string titleStr =
          renderer.truncatedText(BOOKERLY_12_FONT_ID, overlayInfo.title.c_str(), availableWidth, EpdFontFamily::BOLD);
      renderer.drawCenteredText(BOOKERLY_12_FONT_ID, currentY, titleStr.c_str(), textBlack, EpdFontFamily::BOLD);
      currentY += lineHeight12 + lineSpacing;
    }

    if (hasAuthor) {
      const std::string authorStr = renderer.truncatedText(UI_10_FONT_ID, overlayInfo.author.c_str(), availableWidth);
      renderer.drawCenteredText(UI_10_FONT_ID, currentY, authorStr.c_str(), textBlack);
      currentY += lineHeight10 + sectionSpacing;
    }

    if (hasProgress) {
      std::string progressStr;
      if (!overlayInfo.chapterName.empty()) {
        const std::string prefix = "Chapter: ";
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

  // Check if the current book is XTC, TXT, or EPUB
  if (FsHelpers::hasXtcExtension(APP_STATE.openEpubPath)) {
    // Handle XTC file
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
  } else if (FsHelpers::hasTxtExtension(APP_STATE.openEpubPath)) {
    // Handle TXT file - looks for cover image in the same folder
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
  } else if (FsHelpers::hasEpubExtension(APP_STATE.openEpubPath)) {
    // Handle EPUB file
    Epub lastEpub(APP_STATE.openEpubPath, "/.crosspoint");
    // Skip loading css since we only need metadata here
    if (!lastEpub.load(true, true)) {
      LOG_ERR("SLP", "Failed to load last epub");
      return (this->*renderNoCoverSleepScreen)();
    }

    if (!lastEpub.generateCoverBmp(cropped)) {
      LOG_ERR("SLP", "Failed to generate cover bmp");
      return (this->*renderNoCoverSleepScreen)();
    }

    coverBmpPath = lastEpub.getCoverBmpPath(cropped);
  } else {
    return (this->*renderNoCoverSleepScreen)();
  }

  FsFile file;
  if (Storage.openFileForRead("SLP", coverBmpPath, file)) {
    Bitmap bitmap(file);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      LOG_DBG("SLP", "Rendering sleep cover: %s", coverBmpPath.c_str());
      renderBitmapSleepScreen(bitmap);
      file.close();
      return;
    }
    file.close();
  }

  return (this->*renderNoCoverSleepScreen)();
}

void SleepActivity::renderBlankSleepScreen() const {
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}
