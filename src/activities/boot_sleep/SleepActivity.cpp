#include "SleepActivity.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Txt.h>
#include <Xtc.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "Epub/converters/DirectPixelWriter.h"
#include "activities/reader/ReaderUtils.h"
#include "fontIds.h"
#include "images/Logo120.h"

namespace {
constexpr uint8_t SLEEP_FACTORY_INTERNAL_PREFLASH_PASSES = 0;

// Stock V5.5.9 byte-match: black flash then white flash, each via the new
// EInkDisplay::displayBufferPrecondition() path which fires CTRL2=0xF7 (full
// power cycle) and skips the SINGLE_BUFFER_MODE post-RED-sync. Matches stock's
// precondition function at firmware addr 0x42010096 — see
// docs/v559-disassembly-findings.md.
constexpr uint8_t FACTORY_SLEEP_PRECONDITION_COLORS[] = {0x00, 0xFF};

void runFactorySleepPrecondition(const GfxRenderer& renderer) {
  for (const uint8_t color : FACTORY_SLEEP_PRECONDITION_COLORS) {
    renderer.displayBufferPrecondition(color);
  }
}
}  // namespace

void SleepActivity::onEnter() {
  Activity::onEnter();

  if (APP_STATE.lastSleepFromReader) {
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
    renderer.setOrientation(GfxRenderer::Orientation::Portrait);
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

  // Check root for sleep.pxc (preferred) or sleep.bmp before scanning the directory.
  if (Storage.exists("/sleep.pxc")) {
    LOG_DBG("SLP", "Loading: /sleep.pxc");
    if (dir) dir.close();
    if (renderPxcSleepScreen("/sleep.pxc")) {
      return;
    }
    renderDefaultSleepScreen();
    return;
  }

  // Look for sleep.bmp on the root of the sd card to determine if we should
  // render a custom sleep screen instead of the default.
  // This takes priority over the /sleep folder.
  {
    FsFile file;
    if (Storage.openFileForRead("SLP", "/sleep.bmp", file)) {
      Bitmap bitmap(file, true);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        LOG_DBG("SLP", "Loading: /sleep.bmp");
        if (bitmap.hasGreyscale() &&
            SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER) {
          lastGrayscalePath = "/sleep.bmp";
          lastGrayscaleIsPxc = false;
        }
        renderBitmapSleepScreen(bitmap);
        file.close();
        if (dir) dir.close();
        return;
      }
      file.close();
    }
  }

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
    // collect all valid BMP/PXC files
    for (auto dirFile = dir.openNextFile(); dirFile; dirFile = dir.openNextFile()) {
      if (dirFile.isDirectory()) {
        dirFile.close();
        continue;
      }
      dirFile.getName(name, sizeof(name));
      auto filename = std::string(name);
      if (filename[0] == '.') {
        dirFile.close();
        continue;
      }

      const bool isBmp = FsHelpers::hasBmpExtension(filename);
      const bool isPxc = FsHelpers::hasPxcExtension(filename);
      if (!isBmp && !isPxc) {
        LOG_DBG("SLP", "Skipping non-BMP/PXC file: %s", name);
        dirFile.close();
        continue;
      }
      if (isBmp) {
        Bitmap bitmap(dirFile);
        if (bitmap.parseHeaders() != BmpReaderError::Ok) {
          LOG_DBG("SLP", "Skipping invalid BMP file: %s", name);
          dirFile.close();
          continue;
        }
      }
      if (isPxc) {
        uint16_t w, h;
        if (dirFile.read(&w, 2) != 2 || dirFile.read(&h, 2) != 2) {
          LOG_DBG("SLP", "Skipping PXC with unreadable header: %s", name);
          dirFile.close();
          continue;
        }
        const int sw = renderer.getScreenWidth();
        const int sh = renderer.getScreenHeight();
        if (w != sw || h != sh) {
          LOG_DBG("SLP", "Skipping PXC size mismatch %dx%d (screen %dx%d): %s", w, h, sw, sh, name);
          dirFile.close();
          continue;
        }
      }
      files.emplace_back(filename);
      dirFile.close();
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
      LOG_DBG("SLP", "Randomly loading: %s/%s", sleepDir, files[randomFileIndex].c_str());
      delay(100);
      if (FsHelpers::hasPxcExtension(files[randomFileIndex])) {
        dir.close();
        if (!renderPxcSleepScreen(filename)) {
          renderDefaultSleepScreen();
        }
        return;
      }
      FsFile randFile;
      if (Storage.openFileForRead("SLP", filename, randFile)) {
        Bitmap bitmap(randFile, true);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          if (bitmap.hasGreyscale() &&
              SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER) {
            lastGrayscalePath = filename;
            lastGrayscaleIsPxc = false;
          }
          renderBitmapSleepScreen(bitmap);
          randFile.close();
          dir.close();
          return;
        }
        randFile.close();
      }
    }
  }
  if (dir) dir.close();

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

bool SleepActivity::renderPxcSleepScreen(const std::string& path) const {
  FsFile file;
  if (!Storage.openFileForRead("SLP", path, file)) {
    LOG_ERR("SLP", "Cannot open PXC: %s", path.c_str());
    return false;
  }

  uint16_t pxcWidth, pxcHeight;
  if (file.read(&pxcWidth, 2) != 2 || file.read(&pxcHeight, 2) != 2) {
    LOG_ERR("SLP", "PXC header read failed: %s", path.c_str());
    file.close();
    return false;
  }

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  if (pxcWidth != screenWidth || pxcHeight != screenHeight) {
    LOG_ERR("SLP", "PXC size %dx%d does not match screen %dx%d", pxcWidth, pxcHeight, screenWidth, screenHeight);
    file.close();
    return false;
  }

  const uint32_t dataOffset = file.position();  // right after the 4-byte header

  // PXC is always 2-bit grayscale - always use factory LUT
  lastGrayscalePath = path;
  lastGrayscaleIsPxc = true;
  struct PxcCtx {
    FsFile* file;
    uint32_t dataOffset;
    int width, height;
  };
  PxcCtx ctx{&file, dataOffset, pxcWidth, pxcHeight};

  runFactorySleepPrecondition(renderer);
  renderer.renderGrayscaleSinglePass(
      GfxRenderer::GrayscaleDriveMode::FactoryQuality,
      [](const GfxRenderer& r, const void* raw) {
        const auto* c = static_cast<const PxcCtx*>(raw);
        c->file->seek(c->dataOffset);

        const int bpr = (c->width + 3) / 4;
        uint8_t* rowBuf = static_cast<uint8_t*>(malloc(bpr));
        if (!rowBuf) {
          LOG_ERR("SLP", "malloc failed for rowBuf (%d bytes, %dx%d)", bpr, c->width, c->height);
          return;
        }

        DirectPixelWriter pw;
        pw.init(r);

        for (int row = 0; row < c->height; row++) {
          if (c->file->read(rowBuf, bpr) != bpr) break;
          pw.beginRow(row);
          for (int col = 0; col < c->width; col++) {
            const uint8_t pv = (rowBuf[col >> 2] >> (6 - (col & 3) * 2)) & 0x03;
            pw.writePixel(pv);
          }
        }
        free(rowBuf);
      },
      &ctx, nullptr, nullptr, HalDisplay::FULL_REFRESH, SLEEP_FACTORY_INTERNAL_PREFLASH_PASSES);

  file.close();
  return true;
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
      y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
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
    y = (pageHeight - bitmap.getHeight()) / 2;
  }

  LOG_DBG("SLP", "drawing to %d x %d", x, y);

  const bool hasGreyscale = bitmap.hasGreyscale() &&
                            SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER;

  if (hasGreyscale) {
    struct BitmapGrayCtx {
      const Bitmap* bitmap;
      int x, y, maxWidth, maxHeight;
      float cropX, cropY;
    };
    BitmapGrayCtx grayCtx{&bitmap, x, y, pageWidth, pageHeight, cropX, cropY};
    runFactorySleepPrecondition(renderer);
    renderer.renderGrayscaleSinglePass(
        GfxRenderer::GrayscaleDriveMode::FactoryQuality,
        [](const GfxRenderer& r, const void* raw) {
          const auto* c = static_cast<const BitmapGrayCtx*>(raw);
          r.drawBitmap(*c->bitmap, c->x, c->y, c->maxWidth, c->maxHeight, c->cropX, c->cropY);
        },
        &grayCtx, nullptr, nullptr, HalDisplay::FULL_REFRESH, SLEEP_FACTORY_INTERNAL_PREFLASH_PASSES);
  } else {
    renderer.clearScreen();
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    if (SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE) {
      renderer.invertScreen();
    }
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
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
    Xtc lastXtc(APP_STATE.openEpubPath, "/.crosspoint");
    if (!lastXtc.load()) {
      LOG_ERR("SLP", "Failed to load last XTC");
      return (this->*renderNoCoverSleepScreen)();
    }

    if (lastXtc.getBitDepth() == 2) {
      const size_t planeSize = (static_cast<size_t>(lastXtc.getPageWidth()) * lastXtc.getPageHeight() + 7) / 8;
      uint8_t* plane1 = static_cast<uint8_t*>(malloc(planeSize));
      if (!plane1) {
        LOG_ERR("SLP", "Failed to alloc plane1 for direct XTCH render (%lu bytes)",
                static_cast<unsigned long>(planeSize));
        return (this->*renderNoCoverSleepScreen)();
      }
      uint8_t* plane2 = static_cast<uint8_t*>(malloc(planeSize));
      if (!plane2) {
        LOG_ERR("SLP", "Failed to alloc plane2 for direct XTCH render (%lu bytes)",
                static_cast<unsigned long>(planeSize));
        free(plane1);
        return (this->*renderNoCoverSleepScreen)();
      }

      if (lastXtc.loadPageMsb(0, plane1, planeSize) == 0) {
        LOG_ERR("SLP", "Failed to load XTCH plane1 for sleep cover");
        free(plane1);
        free(plane2);
        return (this->*renderNoCoverSleepScreen)();
      }
      if (lastXtc.loadPageLsb(0, plane2, planeSize) == 0) {
        LOG_ERR("SLP", "Failed to load XTCH plane2 for sleep cover");
        free(plane1);
        free(plane2);
        return (this->*renderNoCoverSleepScreen)();
      }

      LOG_DBG("SLP", "Direct XTCH plane render: %ux%u", lastXtc.getPageWidth(), lastXtc.getPageHeight());
      runFactorySleepPrecondition(renderer);
      renderer.displayXtchPlanes(plane1, plane2, lastXtc.getPageWidth(), lastXtc.getPageHeight(), nullptr, nullptr,
                                 GfxRenderer::GrayscaleDriveMode::FactoryQuality, false);
      free(plane1);
      free(plane2);
      return;
    }

    if (lastXtc.getBitDepth() == 1) {
      const size_t bufferSize = (static_cast<size_t>(lastXtc.getPageWidth() + 7) / 8) * lastXtc.getPageHeight();
      uint8_t* pageBuffer = static_cast<uint8_t*>(malloc(bufferSize));
      if (!pageBuffer) {
        LOG_ERR("SLP", "Failed to alloc page buffer for direct XTC render (%lu bytes)",
                static_cast<unsigned long>(bufferSize));
        return (this->*renderNoCoverSleepScreen)();
      }
      if (lastXtc.loadPage(0, pageBuffer, bufferSize) == 0) {
        LOG_ERR("SLP", "Failed to load XTC page for sleep cover");
        free(pageBuffer);
        return (this->*renderNoCoverSleepScreen)();
      }
      LOG_DBG("SLP", "Direct XTC page render: %ux%u", lastXtc.getPageWidth(), lastXtc.getPageHeight());
      if (!APP_STATE.lastSleepFromReader) {
        renderer.clearScreen();
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      }
      renderer.displayXtcBwPage(pageBuffer, lastXtc.getPageWidth(), lastXtc.getPageHeight());
      free(pageBuffer);
      return;
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
      if (bitmap.hasGreyscale() &&
          SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER) {
        lastGrayscalePath = coverBmpPath;
        lastGrayscaleIsPxc = false;
      }
      renderBitmapSleepScreen(bitmap);
      return;
    }
  }

  return (this->*renderNoCoverSleepScreen)();
}

void SleepActivity::renderBlankSleepScreen() const {
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void SleepActivity::onScreenshotRequest() {
  if (lastGrayscalePath.empty()) return;
  if (lastGrayscaleIsPxc) {
    if (!renderPxcSleepScreen(lastGrayscalePath)) {
      renderDefaultSleepScreen();
    }
  } else {
    FsFile file;
    if (Storage.openFileForRead("SLP", lastGrayscalePath.c_str(), file)) {
      Bitmap bitmap(file, true);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        renderBitmapSleepScreen(bitmap);
      }
      file.close();
    }
  }
  // Device enters deep sleep next; on wake the new activity will full-refresh anyway.
  renderer.clearScreen();
  renderer.cleanupGrayscaleWithFrameBuffer();
}
