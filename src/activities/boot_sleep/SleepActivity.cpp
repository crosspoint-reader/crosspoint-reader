#include "SleepActivity.h"

#include <Epub.h>
#include <GfxRenderer.h>
#include <SDCardManager.h>
#include <Txt.h>
#include <Xtc.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "fontIds.h"
#include "images/CrossLarge.h"
#include "util/StringUtils.h"

void SleepActivity::onEnter() {
  Activity::onEnter();
  renderPopup("Entering Sleep...");

  if (SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::BLANK) {
    return renderBlankSleepScreen();
  }

  if (SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM) {
    return renderCustomSleepScreen();
  }

  if (SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::COVER) {
    return renderCoverSleepScreen();
  }

  renderDefaultSleepScreen();
}

void SleepActivity::renderPopup(const char* message) const {
  const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, message, EpdFontFamily::BOLD);
  constexpr int margin = 20;
  const int x = (renderer.getScreenWidth() - textWidth - margin * 2) / 2;
  constexpr int y = 117;
  const int w = textWidth + margin * 2;
  const int h = renderer.getLineHeight(UI_12_FONT_ID) + margin * 2;
  // renderer.clearScreen();
  renderer.fillRect(x - 5, y - 5, w + 10, h + 10, true);
  renderer.fillRect(x + 5, y + 5, w - 10, h - 10, false);
  renderer.drawText(UI_12_FONT_ID, x + margin, y + margin, message, true, EpdFontFamily::BOLD);
  renderer.displayBuffer();
}

void SleepActivity::renderCustomSleepScreen() const {
  // Check if we have a /sleep directory
  auto dir = SdMan.open("/sleep");
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
      if (SdMan.openFileForRead("SLP", filename, file)) {
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
  if (SdMan.openFileForRead("SLP", "/sleep.bmp", file)) {
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
  renderer.drawImage(CrossLarge, (pageWidth + 128) / 2, (pageHeight - 128) / 2, 128, 128);
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 70, "CrossPoint", true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 95, "SLEEPING");

  // Make sleep screen dark unless light is selected in settings
  if (SETTINGS.sleepScreen != CrossPointSettings::SLEEP_SCREEN_MODE::LIGHT) {
    renderer.invertScreen();
  }

  renderer.displayBuffer(EInkDisplay::HALF_REFRESH);
}

void SleepActivity::renderBitmapSleepScreen(const Bitmap& bitmap) const {
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
      x = std::round((pageWidth - pageHeight * ratio) / 2);
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
  renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
  renderer.displayBuffer(EInkDisplay::HALF_REFRESH);

  if (bitmap.hasGreyscale()) {
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
  if (APP_STATE.openEpubPath.empty()) {
    return renderDefaultSleepScreen();
  }

  std::string coverBmpPath;
  bool cropped = SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP;

  // Check if the current book is XTC, TXT, or EPUB
  if (StringUtils::checkFileExtension(APP_STATE.openEpubPath, ".xtc") ||
      StringUtils::checkFileExtension(APP_STATE.openEpubPath, ".xtch")) {
    // Handle XTC file
    Xtc lastXtc(APP_STATE.openEpubPath, "/.crosspoint");
    if (!lastXtc.load()) {
      Serial.println("[SLP] Failed to load last XTC");
      return renderDefaultSleepScreen();
    }

    if (!lastXtc.generateCoverBmp()) {
      Serial.println("[SLP] Failed to generate XTC cover bmp");
      return renderDefaultSleepScreen();
    }

    coverBmpPath = lastXtc.getCoverBmpPath();
  } else if (StringUtils::checkFileExtension(APP_STATE.openEpubPath, ".txt")) {
    // Handle TXT file - looks for cover image in the same folder
    Txt lastTxt(APP_STATE.openEpubPath, "/.crosspoint");
    if (!lastTxt.load()) {
      Serial.println("[SLP] Failed to load last TXT");
      return renderDefaultSleepScreen();
    }

    if (!lastTxt.generateCoverBmp()) {
      Serial.println("[SLP] No cover image found for TXT file");
      return renderDefaultSleepScreen();
    }

    coverBmpPath = lastTxt.getCoverBmpPath();
  } else if (StringUtils::checkFileExtension(APP_STATE.openEpubPath, ".epub")) {
    // Handle EPUB file
    Epub lastEpub(APP_STATE.openEpubPath, "/.crosspoint");
    if (!lastEpub.load()) {
      Serial.println("[SLP] Failed to load last epub");
      return renderDefaultSleepScreen();
    }

    if (!lastEpub.generateCoverBmp(cropped)) {
      Serial.println("[SLP] Failed to generate cover bmp");
      return renderDefaultSleepScreen();
    }

    coverBmpPath = lastEpub.getCoverBmpPath(cropped);
  } else {
    return renderDefaultSleepScreen();
  }

  FsFile file;
  if (SdMan.openFileForRead("SLP", coverBmpPath, file)) {
    Bitmap bitmap(file);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      renderBitmapSleepScreen(bitmap);
      return;
    }
  }

  renderDefaultSleepScreen();
}

void SleepActivity::renderBlankSleepScreen() const {
  renderer.clearScreen();
  renderer.displayBuffer(EInkDisplay::HALF_REFRESH);
}
