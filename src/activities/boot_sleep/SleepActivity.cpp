#include "SleepActivity.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <PNGdec.h>
#include <Serialization.h>
#include <Txt.h>
#include <Xtc.h>
#include <esp_system.h>

#include <algorithm>
#include <new>

#include "../reader/EpubReaderActivity.h"
#include "../reader/TxtReaderActivity.h"
#include "../reader/XtcReaderActivity.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/Logo120.h"


namespace {

// Context passed through PNGdec's decode() user-pointer to the per-scanline draw callback.
struct PngOverlayCtx {
  const GfxRenderer* renderer;
  int screenW;
  int screenH;
  int srcWidth;
  int dstWidth;
  int dstX;
  int dstY;
  float yScale;
  int lastDstY;
  // Color-key transparency (tRNS chunk) for TRUECOLOR and GRAYSCALE images.
  // Initialized lazily on the first draw callback because tRNS is processed during decode(),
  // not during open() — so hasAlpha()/getTransparentColor() are only valid once decode() starts.
  // -2 = not yet read; -1 = no color key; >=0 = 0x00RRGGBB (TRUECOLOR) or low-byte gray.
  int32_t transparentColor;
  PNG* pngObj;  // for lazy-init of transparentColor on first callback
};

// PNGdec file I/O callbacks — mirror the pattern in PngToFramebufferConverter.cpp.
void* pngSleepOpen(const char* filename, int32_t* size) {
  FsFile* f = new FsFile();
  if (!Storage.openFileForRead("SLP", std::string(filename), *f)) {
    delete f;
    return nullptr;
  }
  *size = f->size();
  return f;
}
void pngSleepClose(void* handle) {
  FsFile* f = reinterpret_cast<FsFile*>(handle);
  if (f) {
    f->close();
    delete f;
  }
}
int32_t pngSleepRead(PNGFILE* pFile, uint8_t* pBuf, int32_t len) {
  FsFile* f = reinterpret_cast<FsFile*>(pFile->fHandle);
  return f ? f->read(pBuf, len) : 0;
}
int32_t pngSleepSeek(PNGFILE* pFile, int32_t pos) {
  FsFile* f = reinterpret_cast<FsFile*>(pFile->fHandle);
  if (!f) return -1;
  return f->seek(pos);
}

// Per-scanline draw callback for PNG overlay compositing.
// Transparent pixels (alpha < 128) are skipped so the reader page shows through.
// Opaque pixels are drawn in their grayscale brightness (dark → black, light → white).
int pngOverlayDraw(PNGDRAW* pDraw) {
  PngOverlayCtx* ctx = reinterpret_cast<PngOverlayCtx*>(pDraw->pUser);

  // Lazy-init: tRNS chunk is processed during decode() before any IDAT data, so by the time
  // the first draw callback fires, hasAlpha() / getTransparentColor() are already valid.
  if (ctx->transparentColor == -2) {
    const int pt = pDraw->iPixelType;
    ctx->transparentColor = (pDraw->iHasAlpha && (pt == PNG_PIXEL_TRUECOLOR || pt == PNG_PIXEL_GRAYSCALE))
                                ? (int32_t)ctx->pngObj->getTransparentColor()
                                : -1;
  }

  const int destY = ctx->dstY + (int)(pDraw->y * ctx->yScale);
  if (destY == ctx->lastDstY) return 1;  // skip duplicate rows from Y scaling
  ctx->lastDstY = destY;
  if (destY < 0 || destY >= ctx->screenH) return 1;

  const int srcWidth = ctx->srcWidth;
  const int dstWidth = ctx->dstWidth;
  const uint8_t* pixels = pDraw->pPixels;
  const int pixelType = pDraw->iPixelType;
  const int hasAlpha = pDraw->iHasAlpha;

  int srcX = 0, error = 0;
  for (int dstX = 0; dstX < dstWidth; dstX++) {
    const int outX = ctx->dstX + dstX;
    if (outX >= 0 && outX < ctx->screenW) {
      uint8_t alpha = 255, gray = 0;
      switch (pixelType) {
        case PNG_PIXEL_TRUECOLOR_ALPHA: {
          const uint8_t* p = &pixels[srcX * 4];
          alpha = p[3];
          gray = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
          break;
        }
        case PNG_PIXEL_GRAY_ALPHA:
          gray = pixels[srcX * 2];
          alpha = pixels[srcX * 2 + 1];
          break;
        case PNG_PIXEL_TRUECOLOR: {
          const uint8_t* p = &pixels[srcX * 3];
          gray = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
          // tRNS color-key: if pixel matches the designated transparent color, skip it
          if (ctx->transparentColor >= 0 && p[0] == (uint8_t)((ctx->transparentColor >> 16) & 0xFF) &&
              p[1] == (uint8_t)((ctx->transparentColor >> 8) & 0xFF) &&
              p[2] == (uint8_t)(ctx->transparentColor & 0xFF)) {
            alpha = 0;
          }
          break;
        }
        case PNG_PIXEL_GRAYSCALE:
          gray = pixels[srcX];
          // tRNS color-key: transparent gray value stored in low byte
          if (ctx->transparentColor >= 0 && gray == (uint8_t)(ctx->transparentColor & 0xFF)) {
            alpha = 0;
          }
          break;
        case PNG_PIXEL_INDEXED:
          if (pDraw->pPalette) {
            const uint8_t idx = pixels[srcX];
            const uint8_t* p = &pDraw->pPalette[idx * 3];
            gray = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
            if (hasAlpha) alpha = pDraw->pPalette[768 + idx];
          }
          break;
        default:
          gray = pixels[srcX];
          break;
      }

      if (alpha >= 128) {
        ctx->renderer->drawPixel(outX, destY, gray < 128);  // true = black, false = white
      }
      // alpha < 128: transparent — leave the reader page pixel intact
    }

    // Bresenham-style X stepping (handles downscaling; 1:1 when srcWidth == dstWidth)
    error += srcWidth;
    while (error >= dstWidth) {
      error -= dstWidth;
      srcX++;
    }
  }
  return 1;
}

// Collects full paths of valid image files from /.sleep and /sleep, with no preference between
// the two directories. BMP files are validated by parsing their headers; invalid BMPs are skipped.
// When allowPng is true, .png files are also accepted (PNG validation happens later at decode time).
std::vector<std::string> collectSleepImages(bool allowPng) {
  std::vector<std::string> files;
  for (const char* sleepDir : {"/.sleep", "/sleep"}) {
    auto dir = Storage.open(sleepDir);
    if (!dir || !dir.isDirectory()) {
      if (dir) dir.close();
      continue;
    }
    char name[500];
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
      const bool isBmp = FsHelpers::hasBmpExtension(filename);
      const bool isPng = allowPng && FsHelpers::hasPngExtension(filename);
      if (!isBmp && !isPng) {
        file.close();
        continue;
      }
      if (isBmp) {
        Bitmap bmp(file);
        if (bmp.parseHeaders() != BmpReaderError::Ok) {
          LOG_DBG("SLP", "Skipping invalid BMP file: %s", name);
          file.close();
          continue;
        }
      }
      files.emplace_back(std::string(sleepDir) + "/" + filename);
      file.close();
    }
    dir.close();
  }
  // Sort by full path so the order is deterministic across reboots — required for sequential
  // pick mode, harmless for random pick mode.
  std::sort(files.begin(), files.end());
  return files;
}

// Picks the next file index based on the user's pick mode.
// RANDOM: uniform random with single reroll to avoid immediate repeats.
// SEQUENTIAL: advances from APP_STATE.lastSleepImage, wrapping at numFiles.
size_t pickSleepImageIndex(size_t numFiles) {
  if (SETTINGS.sleepImagePickMode == CrossPointSettings::SLEEP_IMAGE_PICK_MODE::PICK_SEQUENTIAL) {
    const size_t last = APP_STATE.lastSleepImage;
    if (last == SIZE_MAX || last >= numFiles) return 0;
    return (last + 1) % numFiles;
  }
  size_t idx = static_cast<size_t>(esp_random() % numFiles);
  while (numFiles > 1 && APP_STATE.lastSleepImage != SIZE_MAX && idx == APP_STATE.lastSleepImage) {
    idx = static_cast<size_t>(esp_random() % numFiles);
  }
  return idx;
}

}  // namespace

void SleepActivity::onEnter() {
  Activity::onEnter();
  // For OVERLAY mode the popup is suppressed so the frame buffer (reader page) stays intact
  if (SETTINGS.sleepScreen != CrossPointSettings::SLEEP_SCREEN_MODE::OVERLAY) {
    GUI.drawPopup(renderer, tr(STR_ENTERING_SLEEP));
  }

  switch (SETTINGS.sleepScreen) {
    case (CrossPointSettings::SLEEP_SCREEN_MODE::BLANK):
      return renderBlankSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM):
      return renderCustomSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER):
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM):
      return renderCoverSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::OVERLAY):
      return renderOverlaySleepScreen();
    default:
      return renderDefaultSleepScreen();
  }
}

void SleepActivity::renderCustomSleepScreen() const {
  const BookOverlayInfo overlayInfo{};
  const bool shouldLoadOverlayInfo =
      SETTINGS.sleepCoverOverlay != 0 && APP_STATE.lastSleepFromReader && !APP_STATE.openEpubPath.empty();

  // An explicitly selected custom sleep image should override random images from /.sleep or /sleep.
  FsFile explicitSleepFile;
  if (Storage.openFileForRead("SLP", "/sleep.bmp", explicitSleepFile)) {
    Bitmap bitmap(explicitSleepFile, true);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      LOG_DBG("SLP", "Loading explicit custom sleep image: /sleep.bmp");
      const BookOverlayInfo resolvedOverlayInfo =
          shouldLoadOverlayInfo ? getBookOverlayInfo(APP_STATE.openEpubPath) : overlayInfo;
      renderBitmapSleepScreen(bitmap, resolvedOverlayInfo);
      explicitSleepFile.close();
      return;
    }
    explicitSleepFile.close();
  }

  // Collect valid BMP files from both /.sleep and /sleep directories (no preference between them)
  const auto files = collectSleepImages(/*allowPng=*/false);
  const auto numFiles = files.size();
  if (numFiles > 0) {
    const auto pickedIndex = pickSleepImageIndex(numFiles);
    APP_STATE.lastSleepImage = pickedIndex;
    APP_STATE.saveToFile();
    const auto& filename = files[pickedIndex];
    FsFile file;
    if (Storage.openFileForRead("SLP", filename, file)) {
      LOG_DBG("SLP", "Loading sleep image: %s", filename.c_str());
      delay(100);
      Bitmap bitmap(file, true);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        const BookOverlayInfo resolvedOverlayInfo =
            shouldLoadOverlayInfo ? getBookOverlayInfo(APP_STATE.openEpubPath) : overlayInfo;
        renderBitmapSleepScreen(bitmap, resolvedOverlayInfo);
        file.close();
        return;
      }
      file.close();
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
          snprintf(buf, sizeof(buf), tr(STR_OVERLAY_READING_PROGRESS), (unsigned long)currentPage + 1, totalPages,
                   progress);
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
            uint32_t magic;
            serialization::readPod(indexFile, magic);
            uint8_t version;
            serialization::readPod(indexFile, version);
            static constexpr uint32_t INDEX_CACHE_MAGIC = 0x54585449;  // "TXTI"
            static constexpr uint8_t INDEX_CACHE_VERSION = 2;
            if (magic == INDEX_CACHE_MAGIC && version == INDEX_CACHE_VERSION) {
              indexFile.seek(32);
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
    }
  } else if (FsHelpers::checkFileExtension(bookPath, ".epub")) {
    Epub epub(bookPath, "/.crosspoint");
    if (epub.load(true, true)) {
      info.title = epub.getTitle();
      info.author = epub.getAuthor();

      FsFile f;
      if (Storage.openFileForRead("SLP", epub.getCachePath() + "/progress.bin", f)) {
        uint8_t data[6];
        const int dataSize = f.read(data, 6);
        if (dataSize == 4 || dataSize == 6) {
          int currentSpineIndex = data[0] + (data[1] << 8);
          int currentPage = data[2] + (data[3] << 8);
          int pageCount = (dataSize == 6) ? (data[4] + (data[5] << 8)) : 0;
          if (pageCount > 0) {
            float chapterProgress = static_cast<float>(currentPage) / static_cast<float>(pageCount);
            float bookProgress = epub.calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;

            const int tocIndex = epub.getTocIndexForSpineIndex(currentSpineIndex);
            if (tocIndex != -1) {
              const auto tocItem = epub.getTocItem(tocIndex);
              info.chapterName = tocItem.title;
              char suffix[64];
              snprintf(suffix, sizeof(suffix), tr(STR_OVERLAY_CHAPTER_PAGE_SUFFIX), currentPage + 1, pageCount,
                       bookProgress);
              info.progressSuffix = suffix;
              info.progressText = info.chapterName + info.progressSuffix;
            } else {
              char buf[80];
              snprintf(buf, sizeof(buf), tr(STR_OVERLAY_READING_PROGRESS), (unsigned long)currentPage + 1,
                       (unsigned)pageCount, bookProgress);
              info.progressText = buf;
            }
          } else {
            char buf[64];
            snprintf(buf, sizeof(buf), tr(STR_OVERLAY_READING_PROGRESS_NO_TOTAL), (unsigned long)currentPage + 1);
            info.progressText = buf;
          }
        }
        f.close();
      }
    }
  }

  return info;
}

void SleepActivity::renderBitmapSleepScreen(const Bitmap& bitmap, const BookOverlayInfo& overlayInfo) const {
  int x, y;
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  float cropX = 0, cropY = 0;
  const bool topAlignForCoverFit = (SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::COVER ||
                                    SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM) &&
                                   SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::FIT;

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
      y = topAlignForCoverFit
              ? 0
              : std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
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
    y = topAlignForCoverFit ? 0 : (pageHeight - bitmap.getHeight()) / 2;
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

    const int lineHeight12 = renderer.getLineHeight(BOOKERLY_12_FONT_ID);
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

    const bool textBlack = (overlayMode != 3);
    const int topPadding = lineHeight12 / 3;
    const int bottomPadding = lineHeight10 * 2 / 3;
    const int overlayHeight = textBlockHeight + topPadding + bottomPadding;
    const int overlayY = pageHeight - overlayHeight;

    if (overlayMode == 2) {
      renderer.fillRectDither(0, overlayY, pageWidth, overlayHeight, Color::LightGray);
    } else {
      renderer.fillRect(0, overlayY, pageWidth, overlayHeight, overlayMode == 3);
    }

    int currentY = overlayY + topPadding;

    if (hasTitle) {
      const std::string titleStr =
          renderer.truncatedText(BOOKERLY_12_FONT_ID, overlayInfo.title.c_str(), availableWidth, EpdFontFamily::BOLD);
      renderer.drawCenteredText(BOOKERLY_12_FONT_ID, currentY, titleStr.c_str(), textBlack, EpdFontFamily::BOLD);
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
        const std::string prefix = "";
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
      const uint8_t overlayMode = SETTINGS.sleepCoverOverlay;
      const BookOverlayInfo coverOverlayInfo =
          overlayMode != 0 ? getBookOverlayInfo(APP_STATE.openEpubPath) : BookOverlayInfo{};
      renderBitmapSleepScreen(bitmap, coverOverlayInfo);
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

void SleepActivity::renderOverlaySleepScreen() const {
  // Overlay pictures always use portrait orientation regardless of the reader's orientation preference.
  const auto savedOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Portrait);
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Step 1: Ensure the frame buffer contains the reader page.
  // When coming from a reader activity the frame buffer already holds the page.
  // When coming from a non-reader activity we re-render it from the saved progress.
  if (!APP_STATE.lastSleepFromReader && !APP_STATE.openEpubPath.empty()) {
    const auto& path = APP_STATE.openEpubPath;
    bool rendered = false;

    if (FsHelpers::checkFileExtension(path, ".xtc") || FsHelpers::checkFileExtension(path, ".xtch")) {
      rendered = XtcReaderActivity::drawCurrentPageToBuffer(path, renderer);
    } else if (FsHelpers::checkFileExtension(path, ".txt")) {
      rendered = TxtReaderActivity::drawCurrentPageToBuffer(path, renderer);
    } else if (FsHelpers::checkFileExtension(path, ".epub")) {
      rendered = EpubReaderActivity::drawCurrentPageToBuffer(path, renderer);
    }

    if (!rendered) {
      LOG_DBG("SLP", "Page re-render failed, using white background");
      renderer.clearScreen();
    }
  }

  // Step 2: Load the overlay image using the same selection logic as renderCustomSleepScreen.
  // BMP: white pixels are skipped (transparent via drawBitmap), black pixels composited on top.
  // PNG: pixels with alpha < 128 are skipped; opaque pixels are drawn with their grayscale value.
  auto tryDrawOverlay = [&](const std::string& filename) -> bool {
    FsFile file;
    if (!Storage.openFileForRead("SLP", filename, file)) return false;
    Bitmap bitmap(file, true);
    if (bitmap.parseHeaders() != BmpReaderError::Ok) {
      file.close();
      return false;
    }

    int x, y;
    float cropX = 0, cropY = 0;
    if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
      float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
      const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);
      if (ratio > screenRatio) {
        x = 0;
        y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
      } else {
        x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
        y = 0;
      }
    } else {
      x = (pageWidth - bitmap.getWidth()) / 2;
      y = (pageHeight - bitmap.getHeight()) / 2;
    }

    // Draw without clearScreen so the reader page remains in the frame buffer beneath
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    file.close();
    return true;
  };

  auto tryDrawPngOverlay = [&](const std::string& filename) -> bool {
    constexpr size_t MIN_FREE_HEAP = 60 * 1024;  // PNG decoder ~42 KB + overhead
    if (ESP.getFreeHeap() < MIN_FREE_HEAP) {
      LOG_ERR("SLP", "Not enough heap for PNG overlay decoder");
      return false;
    }
    PNG* png = new (std::nothrow) PNG();
    if (!png) return false;

    int rc = png->open(filename.c_str(), pngSleepOpen, pngSleepClose, pngSleepRead, pngSleepSeek, pngOverlayDraw);
    if (rc != PNG_SUCCESS) {
      LOG_DBG("SLP", "PNG open failed: %s (%d)", filename.c_str(), rc);
      delete png;
      return false;
    }

    const int srcW = png->getWidth(), srcH = png->getHeight();
    float yScale = 1.0f;
    int dstW = srcW, dstH = srcH;
    if (srcW > pageWidth || srcH > pageHeight) {
      const float scaleX = (float)pageWidth / srcW, scaleY = (float)pageHeight / srcH;
      const float scale = (scaleX < scaleY) ? scaleX : scaleY;
      dstW = (int)(srcW * scale);
      dstH = (int)(srcH * scale);
      yScale = (float)dstH / srcH;
    }

    PngOverlayCtx ctx;
    ctx.renderer = &renderer;
    ctx.screenW = pageWidth;
    ctx.screenH = pageHeight;
    ctx.srcWidth = srcW;
    ctx.dstWidth = dstW;
    ctx.dstX = (pageWidth - dstW) / 2;
    ctx.dstY = (pageHeight - dstH) / 2;
    ctx.yScale = yScale;
    ctx.lastDstY = -1;
    ctx.transparentColor = -2;  // will be resolved on first draw callback (after tRNS is parsed)
    ctx.pngObj = png;

    rc = png->decode(&ctx, 0);
    png->close();
    delete png;
    return rc == PNG_SUCCESS;
  };

  // Collect images from both /.sleep and /sleep directories (no preference between them).
  // Accepts both .bmp and .png files; .bmp headers are validated during the scan.
  bool overlayDrawn = false;
  const auto files = collectSleepImages(/*allowPng=*/true);
  const auto numFiles = files.size();
  if (numFiles > 0) {
    const auto pickedIndex = pickSleepImageIndex(numFiles);
    APP_STATE.lastSleepImage = pickedIndex;
    APP_STATE.saveToFile();
    const std::string& selected = files[pickedIndex];
    if (FsHelpers::hasPngExtension(selected)) {
      overlayDrawn = tryDrawPngOverlay(selected);
    } else {
      overlayDrawn = tryDrawOverlay(selected);
    }
  }

  if (!overlayDrawn) {
    overlayDrawn = tryDrawOverlay("/sleep.bmp");
  }
  if (!overlayDrawn) {
    overlayDrawn = tryDrawPngOverlay("/sleep.png");
  }

  if (!overlayDrawn) {
    LOG_DBG("SLP", "No overlay image found, displaying page without overlay");
  }

  renderer.setOrientation(savedOrientation);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}
