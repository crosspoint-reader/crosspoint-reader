#include "SleepActivity.h"

#include <Epub.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Txt.h>
#include <Xtc.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/Logo120.h"
#include "util/StringUtils.h"

void SleepActivity::onEnter() {
  Activity::onEnter();
  GUI.drawPopup(renderer, "Entering Sleep...");

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
  // Check if we have a /sleep directory
  auto dir = Storage.open("/sleep");
  if (dir && dir.isDirectory()) {
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

      if (filename.substr(filename.length() - 4) != ".bmp") {
        Serial.printf("[%lu] [SLP] Skipping non-.bmp file name: %s\n", millis(), name);
        file.close();
        continue;
      }
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() != BmpReaderError::Ok) {
        Serial.printf("[%lu] [SLP] Skipping invalid BMP file: %s\n", millis(), name);
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
      while (numFiles > 1 && randomFileIndex == APP_STATE.lastSleepImage) {
        randomFileIndex = random(numFiles);
      }
      APP_STATE.lastSleepImage = randomFileIndex;
      APP_STATE.saveToFile();
      const auto filename = "/sleep/" + files[randomFileIndex];
      FsFile file;
      if (Storage.openFileForRead("SLP", filename, file)) {
        Serial.printf("[%lu] [SLP] Randomly loading: /sleep/%s\n", millis(), files[randomFileIndex].c_str());
        delay(100);
        Bitmap bitmap(file, true);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          renderBitmapSleepScreen(bitmap);
          dir.close();
          return;
        }
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
      Serial.printf("[%lu] [SLP] Loading: /sleep.bmp\n", millis());
      renderBitmapSleepScreen(bitmap);
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
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 70, "CrossPoint", true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 95, "SLEEPING");

  // Make sleep screen dark unless light is selected in settings
  if (SETTINGS.sleepScreen != CrossPointSettings::SLEEP_SCREEN_MODE::LIGHT) {
    renderer.invertScreen();
  }

  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void SleepActivity::renderBitmapSleepScreen(const Bitmap& bitmap, const std::string& overlayText) const {
  int x, y;
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  float cropX = 0, cropY = 0;

  Serial.printf("[%lu] [SLP] bitmap %d x %d, screen %d x %d\n", millis(), bitmap.getWidth(), bitmap.getHeight(),
                pageWidth, pageHeight);
  if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
    // image will scale, make sure placement is right
    float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
    const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

    Serial.printf("[%lu] [SLP] bitmap ratio: %f, screen ratio: %f\n", millis(), ratio, screenRatio);
    if (ratio > screenRatio) {
      // image wider than viewport ratio, scaled down image needs to be centered vertically
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        cropX = 1.0f - (screenRatio / ratio);
        Serial.printf("[%lu] [SLP] Cropping bitmap x: %f\n", millis(), cropX);
        ratio = (1.0f - cropX) * static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
      }
      x = 0;
      y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
      Serial.printf("[%lu] [SLP] Centering with ratio %f to y=%d\n", millis(), ratio, y);
    } else {
      // image taller than viewport ratio, scaled down image needs to be centered horizontally
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        cropY = 1.0f - (ratio / screenRatio);
        Serial.printf("[%lu] [SLP] Cropping bitmap y: %f\n", millis(), cropY);
        ratio = static_cast<float>(bitmap.getWidth()) / ((1.0f - cropY) * static_cast<float>(bitmap.getHeight()));
      }
      x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
      y = 0;
      Serial.printf("[%lu] [SLP] Centering with ratio %f to x=%d\n", millis(), ratio, x);
    }
  } else {
    // center the image
    x = (pageWidth - bitmap.getWidth()) / 2;
    y = (pageHeight - bitmap.getHeight()) / 2;
  }

  Serial.printf("[%lu] [SLP] drawing to %d x %d\n", millis(), x, y);
  renderer.clearScreen();

  const bool hasGreyscale = bitmap.hasGreyscale() &&
                            SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER;

  renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);

  if (SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE) {
    renderer.invertScreen();
  }

  // Draw overlay text if provided
  if (!overlayText.empty()) {
    // Split text into lines
    size_t newlinePos = overlayText.find('\n');
    std::string line1 = (newlinePos != std::string::npos) ? overlayText.substr(0, newlinePos) : overlayText;
    std::string line2 = (newlinePos != std::string::npos && newlinePos + 1 < overlayText.length())
                            ? overlayText.substr(newlinePos + 1)
                            : "";

    // Truncate lines if too long for the screen
    line1 = renderer.truncatedText(UI_10_FONT_ID, line1.c_str(), pageWidth - 20);
    line2 = renderer.truncatedText(UI_10_FONT_ID, line2.c_str(), pageWidth - 20);

    // Draw semi-transparent background rectangle at the bottom (taller for two lines)
    const int overlayHeight = 50;
    const int overlayY = pageHeight - overlayHeight - 20;             // Position 20 pixels higher from bottom
    renderer.fillRect(0, overlayY, pageWidth, overlayHeight, false);  // White background

    // First line - positioned higher in the overlay
    const int line1Y = overlayY + 18;  // Position higher up
    const int textX1 = (pageWidth - renderer.getTextWidth(UI_10_FONT_ID, line1.c_str())) / 2;
    renderer.drawText(UI_10_FONT_ID, textX1, line1Y, line1.c_str(), true);  // Black text

    // Second line - positioned lower in the overlay
    if (!line2.empty()) {
      const int line2Y = overlayY + 38;  // Position lower down with more space
      const int textX2 = (pageWidth - renderer.getTextWidth(UI_10_FONT_ID, line2.c_str())) / 2;
      renderer.drawText(UI_10_FONT_ID, textX2, line2Y, line2.c_str(), true);  // Black text
    }
  }

  renderer.displayBuffer(HalDisplay::HALF_REFRESH);

  if (hasGreyscale) {
    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    renderer.copyGrayscaleLsbBuffers();

    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
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
  if (StringUtils::checkFileExtension(APP_STATE.openEpubPath, ".xtc") ||
      StringUtils::checkFileExtension(APP_STATE.openEpubPath, ".xtch")) {
    // Handle XTC file
    Xtc lastXtc(APP_STATE.openEpubPath, "/.crosspoint");
    if (!lastXtc.load()) {
      Serial.printf("[%lu] [SLP] Failed to load last XTC\n", millis());
      return (this->*renderNoCoverSleepScreen)();
    }

    if (!lastXtc.generateCoverBmp()) {
      Serial.printf("[%lu] [SLP] Failed to generate XTC cover bmp\n", millis());
      return (this->*renderNoCoverSleepScreen)();
    }

    coverBmpPath = lastXtc.getCoverBmpPath();
  } else if (StringUtils::checkFileExtension(APP_STATE.openEpubPath, ".txt")) {
    // Handle TXT file - looks for cover image in the same folder
    Txt lastTxt(APP_STATE.openEpubPath, "/.crosspoint");
    if (!lastTxt.load()) {
      Serial.printf("[%lu] [SLP] Failed to load last TXT\n", millis());
      return (this->*renderNoCoverSleepScreen)();
    }

    if (!lastTxt.generateCoverBmp()) {
      Serial.printf("[%lu] [SLP] No cover image found for TXT file\n", millis());
      return (this->*renderNoCoverSleepScreen)();
    }

    coverBmpPath = lastTxt.getCoverBmpPath();
  } else if (StringUtils::checkFileExtension(APP_STATE.openEpubPath, ".epub")) {
    // Handle EPUB file
    Epub lastEpub(APP_STATE.openEpubPath, "/.crosspoint");
    // Skip loading css since we only need metadata here
    if (!lastEpub.load(true, true)) {
      Serial.printf("[%lu] [SLP] Failed to load last epub\n", millis());
      return (this->*renderNoCoverSleepScreen)();
    }

    if (!lastEpub.generateCoverBmp(cropped)) {
      Serial.printf("[%lu] [SLP] Failed to generate cover bmp\n", millis());
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
      Serial.printf("[%lu] [SLP] Rendering sleep cover: %s\n", millis(), coverBmpPath.c_str());
      std::string overlayText = SETTINGS.sleepScreenOverlay ? getBookOverlayText(APP_STATE.openEpubPath) : "";
      renderBitmapSleepScreen(bitmap, overlayText);
      return;
    }
  }

  return (this->*renderNoCoverSleepScreen)();
}

std::string SleepActivity::getBookOverlayText(const std::string& bookPath) const {
  std::string title;
  std::string author;
  std::string progressLine = "Reading...";  // Default progress text

  // Extract metadata based on file type
  if (StringUtils::checkFileExtension(bookPath, ".xtc") || StringUtils::checkFileExtension(bookPath, ".xtch")) {
    // Handle XTC file
    Xtc xtc(bookPath, "/.crosspoint");
    if (xtc.load()) {
      title = xtc.getTitle();
      author = xtc.getAuthor();

      // Load progress - XTC stores current page
      FsFile f;
      if (Storage.openFileForRead("SLP", xtc.getCachePath() + "/progress.bin", f)) {
        uint8_t data[4];
        if (f.read(data, 4) == 4) {
          uint32_t currentPage = data[0] + (data[1] << 8) + (data[2] << 16) + (data[3] << 24);
          uint32_t totalPages = xtc.getPageCount();
          float progress = xtc.calculateProgress(currentPage) * 100.0f;
          char progressStr[32];
          snprintf(progressStr, sizeof(progressStr), "%lu/%u %.0f%%", (unsigned long)currentPage + 1, totalPages,
                   progress);
          progressLine = progressStr;
        }
        f.close();
      }
    }
  } else if (StringUtils::checkFileExtension(bookPath, ".txt")) {
    // Handle TXT file
    Txt txt(bookPath, "/.crosspoint");
    if (txt.load()) {
      title = txt.getTitle();
      // TXT files don't have author metadata

      // Load progress - TXT stores current page
      FsFile f;
      if (Storage.openFileForRead("SLP", txt.getCachePath() + "/progress.bin", f)) {
        uint8_t data[4];
        if (f.read(data, 4) == 4) {
          uint32_t currentPage = data[0] + (data[1] << 8);

          // Load total pages from page index cache
          uint32_t totalPages = 0;
          FsFile indexFile;
          if (Storage.openFileForRead("SLP", txt.getCachePath() + "/index.bin", indexFile)) {
            // Read numPages directly (it's at a fixed offset after headers)
            // Skip: magic(4) + version(4) + fileSize(4) + cachedWidth(4) + cachedLines(4) + fontId(4) + margin(4) +
            // alignment(4) = 32 bytes
            indexFile.seek(32);
            if (indexFile.read(reinterpret_cast<uint8_t*>(&totalPages), sizeof(totalPages)) == sizeof(totalPages)) {
              // Successfully read totalPages
            }
            indexFile.close();
          }

          if (totalPages > 0) {
            float progress = (currentPage + 1) * 100.0f / totalPages;
            char progressStr[32];
            snprintf(progressStr, sizeof(progressStr), "%lu/%u %.0f%%", (unsigned long)currentPage + 1, totalPages,
                     progress);
            progressLine = progressStr;
          } else {
            char progressStr[16];
            snprintf(progressStr, sizeof(progressStr), "Page %lu", (unsigned long)currentPage + 1);
            progressLine = progressStr;
          }
        }
        f.close();
      }
    }
  } else if (StringUtils::checkFileExtension(bookPath, ".epub")) {
    // Handle EPUB file
    Epub epub(bookPath, "/.crosspoint");
    if (epub.load(true, true)) {  // Skip CSS loading for metadata only
      title = epub.getTitle();
      author = epub.getAuthor();

      // Load progress and get chapter information
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

            // Get chapter name from TOC
            const int tocIndex = epub.getTocIndexForSpineIndex(currentSpineIndex);
            std::string chapterName;
            if (tocIndex == -1) {
              chapterName = "Unnamed Chapter";
            } else {
              const auto tocItem = epub.getTocItem(tocIndex);
              chapterName = tocItem.title;
            }

            // Create chapter info line with chapter name and progress
            char chapterProgressStr[128];
            snprintf(chapterProgressStr, sizeof(chapterProgressStr), "%s: %d/%d %.0f%%", chapterName.c_str(),
                     currentPage + 1, pageCount, bookProgress);
            progressLine = chapterProgressStr;
          }
        }
        f.close();
      }
    }
  }

  // Format overlay text (two lines)
  std::string overlayText;
  if (!title.empty()) {
    overlayText = title;
    if (!author.empty()) {
      overlayText += " - " + author;
    }
    overlayText += "\n";  // New line
    overlayText += progressLine;
  }

  return overlayText;
}

void SleepActivity::renderBlankSleepScreen() const {
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}
