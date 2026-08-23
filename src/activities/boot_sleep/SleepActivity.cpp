#include "SleepActivity.h"

#include <Epub.h>
#include <Epub/converters/PngToFramebufferConverter.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>
#include <PNGdec.h>
#include <Txt.h>
#include <Xtc.h>
#include <Serialization.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/Logo120.h"
#include "images/MoonIcon.h"

namespace {

// Kept separate from /sleep.bmp and /.sleep so alpha-overlay art does not mix with full-screen wallpapers.
constexpr char TRANSPARENT_SLEEP_ROOT_BMP[] = "/sleep-overlay.bmp";
constexpr char TRANSPARENT_SLEEP_ROOT_PNG[] = "/sleep-overlay.png";
constexpr char TRANSPARENT_SLEEP_DIR[] = "/.sleep-overlay";
constexpr char TRANSPARENT_SLEEP_LEGACY_DIR[] = "/sleep-overlay";
constexpr size_t MAX_SLEEP_FILE_NAME_LEN = 256;
constexpr uint8_t MIN_VISIBLE_ALPHA = 8;

struct BitmapPlacement {
  int x = 0;
  int y = 0;
  float cropX = 0.0f;
  float cropY = 0.0f;
};

struct OverlayBmpInfo {
  int width = 0;
  int height = 0;
  bool topDown = false;
  uint32_t dataOffset = 0;
  uint32_t rowBytes = 0;
};

uint16_t readLE16(HalFile& file) {
  const int c0 = file.read();
  const int c1 = file.read();
  const auto b0 = static_cast<uint8_t>(c0 < 0 ? 0 : c0);
  const auto b1 = static_cast<uint8_t>(c1 < 0 ? 0 : c1);
  return static_cast<uint16_t>(b0) | (static_cast<uint16_t>(b1) << 8);
}

uint32_t readLE32(HalFile& file) {
  const int c0 = file.read();
  const int c1 = file.read();
  const int c2 = file.read();
  const int c3 = file.read();
  const auto b0 = static_cast<uint8_t>(c0 < 0 ? 0 : c0);
  const auto b1 = static_cast<uint8_t>(c1 < 0 ? 0 : c1);
  const auto b2 = static_cast<uint8_t>(c2 < 0 ? 0 : c2);
  const auto b3 = static_cast<uint8_t>(c3 < 0 ? 0 : c3);
  return static_cast<uint32_t>(b0) | (static_cast<uint32_t>(b1) << 8) | (static_cast<uint32_t>(b2) << 16) |
         (static_cast<uint32_t>(b3) << 24);
}

uint32_t readBE32(HalFile& file) {
  const int c0 = file.read();
  const int c1 = file.read();
  const int c2 = file.read();
  const int c3 = file.read();
  if (c0 < 0 || c1 < 0 || c2 < 0 || c3 < 0) return 0;
  return (static_cast<uint32_t>(c0) << 24) | (static_cast<uint32_t>(c1) << 16) | (static_cast<uint32_t>(c2) << 8) |
         static_cast<uint32_t>(c3);
}

bool isValidPngHeader(HalFile& file) {
  static constexpr uint8_t PNG_SIGNATURE[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  static constexpr uint32_t MAX_SOURCE_PIXELS = 2048u * 1536u;
  uint8_t signature[8];
  if (!file.seek(0) || file.read(signature, sizeof(signature)) != static_cast<int>(sizeof(signature)) ||
      !std::equal(std::begin(signature), std::end(signature), std::begin(PNG_SIGNATURE))) {
    return false;
  }

  const uint32_t ihdrLength = readBE32(file);
  char chunkType[4];
  if (file.read(reinterpret_cast<uint8_t*>(chunkType), sizeof(chunkType)) != static_cast<int>(sizeof(chunkType)) ||
      ihdrLength != 13 || !std::equal(std::begin(chunkType), std::end(chunkType), "IHDR")) {
    return false;
  }

  const uint32_t width = readBE32(file);
  const uint32_t height = readBE32(file);
  const int bitDepth = file.read();
  const int colorType = file.read();
  const int compression = file.read();
  const int filter = file.read();
  const int interlace = file.read();

  const bool supportedBitDepth =
      bitDepth == 8 || ((colorType == PNG_PIXEL_GRAYSCALE || colorType == PNG_PIXEL_INDEXED) &&
                        (bitDepth == 1 || bitDepth == 2 || bitDepth == 4));
  const bool supportedColorType = colorType == PNG_PIXEL_GRAYSCALE || colorType == PNG_PIXEL_TRUECOLOR ||
                                  colorType == PNG_PIXEL_INDEXED || colorType == PNG_PIXEL_GRAY_ALPHA ||
                                  colorType == PNG_PIXEL_TRUECOLOR_ALPHA;
  return width > 0 && height > 0 && width <= 2048 && height <= 3072 && width * height <= MAX_SOURCE_PIXELS &&
         supportedBitDepth && supportedColorType && compression == 0 && filter == 0 && interlace == 0;
}

BitmapPlacement calculateBitmapPlacement(const int bitmapWidth, const int bitmapHeight, const GfxRenderer& renderer) {
  BitmapPlacement placement;
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  if (bitmapWidth > pageWidth || bitmapHeight > pageHeight) {
    float ratio = static_cast<float>(bitmapWidth) / static_cast<float>(bitmapHeight);
    const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

    if (ratio > screenRatio) {
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        placement.cropX = 1.0f - (screenRatio / ratio);
        ratio = (1.0f - placement.cropX) * static_cast<float>(bitmapWidth) / static_cast<float>(bitmapHeight);
      }
      placement.x = 0;
      placement.y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
    } else {
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        placement.cropY = 1.0f - (ratio / screenRatio);
        ratio = static_cast<float>(bitmapWidth) / ((1.0f - placement.cropY) * static_cast<float>(bitmapHeight));
      }
      placement.x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
      placement.y = 0;
    }
  } else {
    placement.x = (pageWidth - bitmapWidth) / 2;
    placement.y = (pageHeight - bitmapHeight) / 2;
  }

  return placement;
}

bool parseOverlayBmpHeader(HalFile& file, OverlayBmpInfo& info, const bool logErrors) {
  if (!file) return false;
  if (!file.seek(0)) return false;

  if (readLE16(file) != 0x4D42) {
    if (logErrors) LOG_ERR("SLP", "Transparent overlay is not a BMP");
    return false;
  }

  file.seekCur(8);
  info.dataOffset = readLE32(file);

  const uint32_t dibSize = readLE32(file);
  if (dibSize < 40) {
    if (logErrors) LOG_ERR("SLP", "Unsupported BMP DIB header: %u", static_cast<unsigned>(dibSize));
    return false;
  }

  info.width = static_cast<int32_t>(readLE32(file));
  const auto rawHeight = static_cast<int32_t>(readLE32(file));
  if (rawHeight == std::numeric_limits<int32_t>::min()) {
    if (logErrors) LOG_ERR("SLP", "Bad transparent overlay dimensions: %dx%d", info.width, rawHeight);
    return false;
  }
  info.topDown = rawHeight < 0;
  info.height = info.topDown ? -rawHeight : rawHeight;

  const uint16_t planes = readLE16(file);
  const uint16_t bpp = readLE16(file);
  const uint32_t compression = readLE32(file);

  // Match Bitmap::parseHeaders(): accept BI_RGB (0) and 32bpp BI_BITFIELDS (3), but keep the same
  // byte-layout assumption as custom sleep BMPs. The renderer below treats pixels as BGRA and does not parse masks.
  if (planes != 1 || bpp != 32 || !(compression == 0 || compression == 3)) {
    if (logErrors) {
      LOG_ERR("SLP", "Transparent overlay must be 32-bit BGRA BMP (planes=%u bpp=%u comp=%u)", planes, bpp,
              static_cast<unsigned>(compression));
    }
    return false;
  }

  constexpr int MAX_IMAGE_WIDTH = 2048;
  constexpr int MAX_IMAGE_HEIGHT = 3072;
  if (info.width <= 0 || info.height <= 0 || info.width > MAX_IMAGE_WIDTH || info.height > MAX_IMAGE_HEIGHT) {
    if (logErrors) LOG_ERR("SLP", "Bad transparent overlay dimensions: %dx%d", info.width, info.height);
    return false;
  }

  info.rowBytes = static_cast<uint32_t>(info.width) * 4u;
  if (!file.seek(info.dataOffset)) {
    if (logErrors) LOG_ERR("SLP", "Failed to seek transparent overlay pixel data");
    return false;
  }

  return true;
}

uint8_t bayerThreshold4x4(const int x, const int y) {
  static constexpr uint8_t BAYER_4X4[16] = {0, 128, 32, 160, 192, 64, 224, 96, 48, 176, 16, 144, 240, 112, 208, 80};
  return BAYER_4X4[((y & 0x03) << 2) | (x & 0x03)];
}

enum class TransparentOverlayPass : uint8_t { BW, GrayscaleLsb, GrayscaleMsb };

uint8_t quantizeOverlayLum(const uint8_t lum) {
  // Match Bitmap's native-palette path: 0, 85, 170, 255 map directly to levels 0..3.
  return lum >> 6;
}

bool renderTransparentOverlayPass(HalFile& file, const OverlayBmpInfo& info, const BitmapPlacement& placement,
                                  const GfxRenderer& renderer, uint8_t* row, const TransparentOverlayPass pass) {
  if (!file.seek(info.dataOffset)) {
    LOG_ERR("SLP", "Failed to seek transparent overlay pixel data");
    return false;
  }

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const int cropPixX = std::floor(info.width * placement.cropX / 2.0f);
  const int cropPixY = std::floor(info.height * placement.cropY / 2.0f);
  const float croppedWidth = (1.0f - placement.cropX) * static_cast<float>(info.width);
  const float croppedHeight = (1.0f - placement.cropY) * static_cast<float>(info.height);

  float scale = 1.0f;
  if (croppedWidth > 0.0f && croppedHeight > 0.0f) {
    const float widthScale = static_cast<float>(pageWidth) / croppedWidth;
    const float heightScale = static_cast<float>(pageHeight) / croppedHeight;
    scale = std::min(widthScale, heightScale);
    if (scale > 1.0f) scale = 1.0f;
  }
  const bool isScaled = scale < 1.0f;

  for (int bmpY = 0; bmpY < info.height; bmpY++) {
    if (file.read(row, info.rowBytes) != static_cast<int>(info.rowBytes)) {
      LOG_ERR("SLP", "Short read in transparent overlay row %d", bmpY);
      return false;
    }

    int screenY = -cropPixY + (info.topDown ? bmpY : info.height - 1 - bmpY);
    if (isScaled) screenY = std::floor(screenY * scale);
    screenY += placement.y;

    if (screenY >= pageHeight) {
      if (info.topDown) break;
      continue;
    }
    if (screenY < 0) {
      if (!info.topDown) break;
      continue;
    }

    for (int bmpX = cropPixX; bmpX < info.width - cropPixX; bmpX++) {
      int screenX = bmpX - cropPixX;
      if (isScaled) screenX = std::floor(screenX * scale);
      screenX += placement.x;

      if (screenX >= renderer.getScreenWidth()) break;
      if (screenX < 0) continue;

      const uint8_t* pixel = row + (static_cast<size_t>(bmpX) * 4u);
      const uint8_t alpha = pixel[3];
      if (alpha < MIN_VISIBLE_ALPHA || alpha <= bayerThreshold4x4(screenX, screenY)) continue;

      const uint8_t lum = (77u * pixel[2] + 150u * pixel[1] + 29u * pixel[0]) >> 8;
      const uint8_t level = quantizeOverlayLum(lum);

      switch (pass) {
        case TransparentOverlayPass::BW:
          // Same first pass as custom bitmap sleep: all non-white levels are painted black.
          // Transparent overlay's only difference is that opaque white explicitly erases underlying text.
          renderer.drawPixel(screenX, screenY, level < 3);
          break;
        case TransparentOverlayPass::GrayscaleLsb:
          if (level == 1) renderer.drawPixel(screenX, screenY, false);
          break;
        case TransparentOverlayPass::GrayscaleMsb:
          if (level == 1 || level == 2) renderer.drawPixel(screenX, screenY, false);
          break;
      }
    }
  }

  return true;
}

enum class AlphaOverlayResult : uint8_t { Rendered, NotAlphaOverlay, Error };
enum class AlphaScanResult : uint8_t { Useful, NotUseful, Error };

AlphaScanResult scanForUsefulAlpha(HalFile& file, const OverlayBmpInfo& info, uint8_t* row) {
  if (!file.seek(info.dataOffset)) {
    LOG_ERR("SLP", "Failed to seek transparent overlay pixel data");
    return AlphaScanResult::Error;
  }

  bool hasVisiblePixel = false;
  bool hasNonOpaquePixel = false;
  for (int bmpY = 0; bmpY < info.height; bmpY++) {
    if (file.read(row, info.rowBytes) != static_cast<int>(info.rowBytes)) {
      LOG_ERR("SLP", "Short read while checking transparent overlay row %d", bmpY);
      return AlphaScanResult::Error;
    }

    for (int bmpX = 0; bmpX < info.width; bmpX++) {
      const uint8_t alpha = row[static_cast<size_t>(bmpX) * 4u + 3u];
      hasVisiblePixel |= alpha >= MIN_VISIBLE_ALPHA;
      hasNonOpaquePixel |= alpha < 255;
      if (hasVisiblePixel && hasNonOpaquePixel) return AlphaScanResult::Useful;
    }
  }

  return AlphaScanResult::NotUseful;
}

AlphaOverlayResult tryRenderTransparentOverlayBmp(HalFile& file, GfxRenderer& renderer, const char* pathForLog) {
  OverlayBmpInfo info;
  if (!parseOverlayBmpHeader(file, info, false)) return AlphaOverlayResult::NotAlphaOverlay;

  const auto placement = calculateBitmapPlacement(info.width, info.height, renderer);
  auto row = makeUniqueNoThrow<uint8_t[]>(info.rowBytes);
  if (!row) {
    LOG_ERR("SLP", "OOM: transparent overlay row (%u bytes)", static_cast<unsigned>(info.rowBytes));
    return AlphaOverlayResult::Error;
  }

  const auto alphaScanResult = scanForUsefulAlpha(file, info, row.get());
  if (alphaScanResult == AlphaScanResult::Error) return AlphaOverlayResult::Error;
  if (alphaScanResult == AlphaScanResult::NotUseful) return AlphaOverlayResult::NotAlphaOverlay;

  LOG_DBG("SLP", "Rendering transparent overlay: %s (%dx%d)", pathForLog, info.width, info.height);

  if (!renderTransparentOverlayPass(file, info, placement, renderer, row.get(), TransparentOverlayPass::BW))
    return AlphaOverlayResult::Error;
  renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  if (!renderTransparentOverlayPass(file, info, placement, renderer, row.get(), TransparentOverlayPass::GrayscaleLsb)) {
    renderer.setRenderMode(GfxRenderer::BW);
    // The BW composite is already on the panel. Keep it instead of falling
    // through to another overlay with this grayscale work buffer cleared.
    return AlphaOverlayResult::Rendered;
  }
  renderer.copyGrayscaleLsbBuffers();

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  if (!renderTransparentOverlayPass(file, info, placement, renderer, row.get(), TransparentOverlayPass::GrayscaleMsb)) {
    renderer.setRenderMode(GfxRenderer::BW);
    return AlphaOverlayResult::Rendered;
  }
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);
  return AlphaOverlayResult::Rendered;
}

enum class SleepRecentKind : uint8_t { Standard, Overlay };

bool isRecentSleepIndex(const SleepRecentKind recentKind, const uint16_t idx, const uint8_t window) {
  return recentKind == SleepRecentKind::Overlay ? APP_STATE.isRecentOverlaySleep(idx, window)
                                                : APP_STATE.isRecentSleep(idx, window);
}

void pushRecentSleepIndex(const SleepRecentKind recentKind, const uint16_t idx) {
  if (recentKind == SleepRecentKind::Overlay) {
    APP_STATE.pushRecentOverlaySleep(idx);
  } else {
    APP_STATE.pushRecentSleep(idx);
  }
}

bool findNextValidSleepImage(HalFile& dir, const SleepRecentKind recentKind, char* name) {
  for (auto dirFile = dir.openNextFile(); dirFile; dirFile = dir.openNextFile()) {
    if (dirFile.isDirectory()) continue;

    dirFile.getName(name, MAX_SLEEP_FILE_NAME_LEN);
    if (name[0] == '\0' || name[0] == '.') continue;

    const bool isBmp = FsHelpers::hasBmpExtension(name);
    const bool isPng = recentKind == SleepRecentKind::Overlay && FsHelpers::hasPngExtension(std::string_view{name});
    if (!isBmp && !isPng) {
      LOG_DBG("SLP", "Skipping unsupported sleep image: %s", name);
      continue;
    }

    const bool isValid = isBmp ? [&dirFile]() {
      Bitmap bitmap(dirFile);
      return bitmap.parseHeaders() == BmpReaderError::Ok;
    }()
                               : isValidPngHeader(dirFile);
    if (!isValid) {
      LOG_DBG("SLP", "Skipping invalid sleep image: %s", name);
      continue;
    }
    return true;
  }
  return false;
}

bool selectRandomSleepFile(const char* dirPath, const SleepRecentKind recentKind, std::string& selectedPath) {
  auto dir = Storage.open(dirPath);
  if (!dir || !dir.isDirectory()) return false;

  auto name = makeUniqueNoThrow<char[]>(MAX_SLEEP_FILE_NAME_LEN);
  if (!name) {
    LOG_ERR("SLP", "OOM: sleep filename buffer");
    return false;
  }

  uint16_t fileCount = 0;
  while (fileCount < UINT16_MAX && findNextValidSleepImage(dir, recentKind, name.get())) ++fileCount;
  if (fileCount == 0) return false;

  // Pick a random wallpaper, excluding recently shown ones.
  // Window: up to SLEEP_RECENT_COUNT entries, capped at fileCount-1.
  const uint8_t recentFill =
      recentKind == SleepRecentKind::Overlay ? APP_STATE.recentOverlaySleepFill : APP_STATE.recentSleepFill;
  const uint8_t window = static_cast<uint8_t>(std::min<uint16_t>(recentFill, fileCount - 1));
  auto randomFileIndex = static_cast<uint16_t>(random(fileCount));
  for (uint8_t attempt = 0; attempt < 20 && isRecentSleepIndex(recentKind, randomFileIndex, window); attempt++) {
    randomFileIndex = static_cast<uint16_t>(random(fileCount));
  }

  dir.rewindDirectory();
  for (uint16_t index = 0; index <= randomFileIndex; ++index) {
    if (!findNextValidSleepImage(dir, recentKind, name.get())) return false;
  }

  selectedPath.reserve(strlen(dirPath) + 1 + strlen(name.get()));
  selectedPath = dirPath;
  selectedPath += "/";
  selectedPath += name.get();
  pushRecentSleepIndex(recentKind, randomFileIndex);
  APP_STATE.saveToFile();
  return true;
}

bool drawSleepPopupPreservingFrame(GfxRenderer& renderer) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int frameThickness = metrics.popupFrameThickness;
  const int popupY = static_cast<int>(renderer.getScreenHeight() * metrics.popupTopOffsetRatio);
  const int popupHeight = renderer.getLineHeight(UI_12_FONT_ID) + metrics.popupMarginY * 2;
  const int bandTop = std::max(0, popupY - frameThickness);
  const int bandBottom = std::min(renderer.getScreenHeight(), popupY + popupHeight + frameThickness);
  const int bandHeight = bandBottom - bandTop;
  const size_t bandBytes = renderer.getRegionByteSize(0, bandTop, renderer.getScreenWidth(), bandHeight);

  auto savedBand = makeUniqueNoThrow<uint8_t[]>(bandBytes);
  if (!savedBand) {
    LOG_ERR("SLP", "OOM: sleep popup background (%u bytes)", static_cast<unsigned>(bandBytes));
    return false;
  }
  if (!renderer.copyRegionToBuffer(0, bandTop, renderer.getScreenWidth(), bandHeight, savedBand.get(), bandBytes)) {
    LOG_ERR("SLP", "Failed to save sleep popup background");
    return false;
  }

  GUI.drawPopup(renderer, tr(STR_ENTERING_SLEEP));
  if (!renderer.copyBufferToRegion(0, bandTop, renderer.getScreenWidth(), bandHeight, savedBand.get(), bandBytes)) {
    LOG_ERR("SLP", "Failed to restore sleep popup background");
    return false;
  }
  return true;
}

void releaseSdFontCachesForDecode(const GfxRenderer& renderer) {
  if (auto* fcm = renderer.getFontCacheManager()) {
    LOG_DBG("SLP", "Free heap before SD font cache release: %d bytes", ESP.getFreeHeap());
    fcm->releaseSdFontCaches();
    LOG_DBG("SLP", "Free heap before sleep image decode: %d bytes", ESP.getFreeHeap());
  }
}

}  // namespace

void SleepActivity::onEnter() {
  Activity::onEnter();

  const bool frameWasInverted = display.isInverted();

  // Sleep screens always use normal polarity. This activity draws directly
  // from onEnter (outside ActivityManager's per-render polarity resolution),
  // so clear any inversion left over from a night-mode reader render.
  display.setInverted(false);

  const bool renderQuickResume =
      SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME ||
      (fromTimeout &&
       SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT);

  if (renderQuickResume) {
    return renderLastScreenSleepScreen();
  }

  if (SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::TRANSPARENT_CUSTOM) {
    // Transparent mode retains the current framebuffer. Materialize any
    // output-level inversion first so the retained content keeps its visible
    // polarity after the display driver returns to normal.
    if (frameWasInverted) renderer.invertScreen();
    if (APP_STATE.lastSleepFromReader) {
      ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
    }
    drawSleepPopupPreservingFrame(renderer);
    if (APP_STATE.lastSleepFromReader) {
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
    }
    releaseSdFontCachesForDecode(renderer);
    return renderTransparentCustomSleepScreen();
  }

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
    case (CrossPointSettings::SLEEP_SCREEN_MODE::BOOK_PHYSICAL):
      if (APP_STATE.openEpubPath.empty()) {
        return renderDefaultSleepScreen();
      } else {
        return renderBookPhysicalSleepScreen();
      }
    default:
      return renderDefaultSleepScreen();
  }
}

void SleepActivity::renderCustomSleepScreen() const {
  // Look for sleep.bmp on the root of the sd card to determine if we should
  // render a custom sleep screen instead of the default.
  // This takes priority over the /sleep folder.
  HalFile file;
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

  std::string selectedPath;
  if (!selectRandomSleepFile("/.sleep", SleepRecentKind::Standard, selectedPath)) {
    selectRandomSleepFile("/sleep", SleepRecentKind::Standard, selectedPath);
  }

  if (!selectedPath.empty()) {
    HalFile randFile;
    if (Storage.openFileForRead("SLP", selectedPath, randFile)) {
      LOG_DBG("SLP", "Randomly loading: %s", selectedPath.c_str());
      delay(100);
      Bitmap bitmap(randFile, true);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        renderBitmapSleepScreen(bitmap);
        randFile.close();
        return;
      }
      randFile.close();
    }
  }

  renderDefaultSleepScreen();
}

// Sleep screens paint with a single HALF refresh (stock parity): the OEM X4
// firmware's only clean refresh in normal operation is the single-pass 0xD7
// sequence, used once for the sleep image. It never runs the multi-flash GC
// waveform (0xF7) that FULL_REFRESH selects (#2471's blinking complaint).
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

void SleepActivity::renderBitmapSleepScreen(const Bitmap& bitmap, const bool preserveBackground) const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto placement = calculateBitmapPlacement(bitmap.getWidth(), bitmap.getHeight(), renderer);
  const int x = placement.x;
  const int y = placement.y;
  const float cropX = placement.cropX;
  const float cropY = placement.cropY;

  LOG_DBG("SLP", "bitmap %d x %d, screen %d x %d", bitmap.getWidth(), bitmap.getHeight(), pageWidth, pageHeight);
  LOG_DBG("SLP", "drawing to %d x %d", x, y);
  if (!preserveBackground) renderer.clearScreen();

  const bool hasGreyscale =
      bitmap.hasGreyscale() && (preserveBackground || SETTINGS.sleepScreenCoverFilter ==
                                                          CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER);

  renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);

  if (!preserveBackground &&
      SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE) {
    renderer.invertScreen();
  }

  if (hasGreyscale) {
    // OEM grayscale pipeline base. Must stay HALF: the gray nudge LUT is
    // calibrated against the pixel state the single-pass HALF waveform leaves
    // behind. A FULL (GC) base parks pixels in a different charge state and
    // the differential nudge then lands unevenly (blotchy noise in gray areas).
    renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);
  } else {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }

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

bool SleepActivity::renderSleepOverlayFile(HalFile& file, const char* pathForLog) const {
  const auto alphaResult = tryRenderTransparentOverlayBmp(file, renderer, pathForLog);
  if (alphaResult == AlphaOverlayResult::Rendered) return true;
  if (alphaResult == AlphaOverlayResult::Error) return false;

  Bitmap bitmap(file);
  const auto parseResult = bitmap.parseHeaders();
  if (parseResult != BmpReaderError::Ok) {
    LOG_ERR("SLP", "Invalid sleep overlay BMP %s: %s", pathForLog, Bitmap::errorToString(parseResult));
    return false;
  }

  LOG_DBG("SLP", "Rendering regular BMP sleep overlay: %s (%dx%d)", pathForLog, bitmap.getWidth(), bitmap.getHeight());
  // drawBitmap leaves white pixels untouched; skipping the initial clear makes
  // them transparent while retaining the existing grayscale pipeline.
  renderBitmapSleepScreen(bitmap, true);
  return true;
}

bool SleepActivity::renderTransparentOverlayPng(const std::string& path) const {
  ImageDimensions dimensions;
  if (!PngToFramebufferConverter::getDimensionsStatic(path, dimensions)) return false;

  const auto placement = calculateBitmapPlacement(dimensions.width, dimensions.height, renderer);
  RenderConfig config;
  config.x = placement.x;
  config.y = placement.y;
  config.maxWidth = renderer.getScreenWidth();
  config.maxHeight = renderer.getScreenHeight();
  config.useDithering = false;
  config.sourceCropX = placement.cropX;
  config.sourceCropY = placement.cropY;
  config.useExactDimensions = placement.cropX > 0.0f || placement.cropY > 0.0f;
  config.preserveAlpha = true;

  PngToFramebufferConverter converter;
  LOG_DBG("SLP", "Rendering transparent PNG overlay: %s (%dx%d)", path.c_str(), dimensions.width, dimensions.height);

  if (!converter.decodeToFramebuffer(path, renderer, config)) return false;
  renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  if (!converter.decodeToFramebuffer(path, renderer, config)) {
    renderer.setRenderMode(GfxRenderer::BW);
    return true;
  }
  renderer.copyGrayscaleLsbBuffers();

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  if (!converter.decodeToFramebuffer(path, renderer, config)) {
    renderer.setRenderMode(GfxRenderer::BW);
    return true;
  }
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);
  return true;
}

bool SleepActivity::renderSleepOverlayPath(const std::string& path) const {
  if (FsHelpers::hasPngExtension(path)) {
    return Storage.exists(path.c_str()) && renderTransparentOverlayPng(path);
  }

  HalFile file;
  return Storage.openFileForRead("SLP", path, file) && renderSleepOverlayFile(file, path.c_str());
}

void SleepActivity::renderTransparentCustomSleepScreen() const {
  if (renderSleepOverlayPath(TRANSPARENT_SLEEP_ROOT_BMP)) return;
  if (renderSleepOverlayPath(TRANSPARENT_SLEEP_ROOT_PNG)) return;

  std::string selectedPath;
  if (!selectRandomSleepFile(TRANSPARENT_SLEEP_DIR, SleepRecentKind::Overlay, selectedPath)) {
    selectRandomSleepFile(TRANSPARENT_SLEEP_LEGACY_DIR, SleepRecentKind::Overlay, selectedPath);
  }

  if (!selectedPath.empty() && renderSleepOverlayPath(selectedPath)) return;

  LOG_ERR("SLP", "No valid transparent sleep overlay found");
  renderDefaultSleepScreen();
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

  HalFile file;
  if (Storage.openFileForRead("SLP", coverBmpPath, file)) {
    Bitmap bitmap(file);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      LOG_DBG("SLP", "Rendering sleep cover: %s", coverBmpPath.c_str());
      renderBitmapSleepScreen(bitmap);
      return;
    }
  }

  return (this->*renderNoCoverSleepScreen)();
}

void SleepActivity::renderLastScreenSleepScreen() const {
  const auto pageHeight = renderer.getScreenHeight();
  renderer.drawImage(MoonIcon, 0, pageHeight - MOONICON_HEIGHT, MOONICON_WIDTH, MOONICON_HEIGHT);
  if (gpio.deviceIsX3()) {
    // The controller still holds the displayed page, so its differential base
    // waveform can add the moon without a full-screen flash.
    renderer.displayGrayscaleBase(HalDisplay::FAST_REFRESH);
  } else {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }
}

void SleepActivity::renderBlankSleepScreen() const {
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

namespace {

struct BookProgressInfo {
  float progress = 0.0f;
  uint32_t estimatedLength = 0;
  bool success = false;
};

BookProgressInfo getBookProgressInfo() {
  BookProgressInfo info;
  if (APP_STATE.openEpubPath.empty()) {
    return info;
  }

  if (FsHelpers::hasXtcExtension(APP_STATE.openEpubPath)) {
    Xtc xtc(APP_STATE.openEpubPath, "/.crosspoint");
    if (xtc.load()) {
      uint32_t currentPage = 0;
      HalFile f;
      if (Storage.openFileForRead("SLP", xtc.getCachePath() + "/progress.bin", f)) {
        uint8_t data[4];
        if (f.read(data, 4) == 4) {
          currentPage = data[0] + (data[1] << 8);
        }
      }
      uint32_t totalPages = xtc.getPageCount();
      if (totalPages > 0) {
        info.progress = static_cast<float>(currentPage) / totalPages;
      }
      info.estimatedLength = totalPages * 2000;
      info.success = true;
    }
  } else if (FsHelpers::hasTxtExtension(APP_STATE.openEpubPath)) {
    Txt txt(APP_STATE.openEpubPath, "/.crosspoint");
    if (txt.load()) {
      uint32_t currentPage = 0;
      HalFile f;
      if (Storage.openFileForRead("SLP", txt.getCachePath() + "/progress.bin", f)) {
        uint8_t data[4];
        if (f.read(data, 4) == 4) {
          currentPage = data[0] + (data[1] << 8);
        }
      }
      uint32_t totalPages = 0;
      HalFile indexFile;
      if (Storage.openFileForRead("SLP", txt.getCachePath() + "/index.bin", indexFile)) {
        uint8_t header[30];
        if (indexFile.read(header, 30) >= 30) {
          totalPages = header[26] + (header[27] << 8) + (header[28] << 16) + (header[29] << 24);
        }
      }
      if (totalPages > 0) {
        info.progress = static_cast<float>(currentPage) / totalPages;
      }
      info.estimatedLength = txt.getFileSize();
      info.success = true;
    }
  } else if (FsHelpers::hasEpubExtension(APP_STATE.openEpubPath)) {
    Epub epub(APP_STATE.openEpubPath, "/.crosspoint");
    if (epub.load(true, true)) {
      uint32_t currentSpineIndex = 0;
      uint32_t nextPageNumber = 0;
      uint32_t cachedChapterTotalPageCount = 0;

      HalFile f;
      if (Storage.openFileForRead("SLP", epub.getCachePath() + "/progress.bin", f)) {
        uint8_t data[10];
        int dataSize = f.read(data, sizeof(data));
        if (dataSize >= 4) {
          currentSpineIndex = data[0] + (data[1] << 8);
          nextPageNumber = data[2] + (data[3] << 8);
        }
        if (dataSize == 6 || dataSize == 10) {
          cachedChapterTotalPageCount = data[4] + (data[5] << 8);
        }
      }

      float chapterProgress = 0.0f;
      if (cachedChapterTotalPageCount > 0) {
        chapterProgress = static_cast<float>(nextPageNumber) / cachedChapterTotalPageCount;
      }

      info.progress = epub.calculateProgress(currentSpineIndex, chapterProgress);
      info.estimatedLength = epub.getBookSize();
      info.success = true;
    }
  }
  return info;
}

} // namespace

void SleepActivity::renderBookPhysicalSleepScreen() const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // 1. Get progress and book length
  BookProgressInfo info = getBookProgressInfo();

  // 2. Load cover if exists
  std::string coverBmpPath;
  bool cropped = SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP;
  if (!APP_STATE.openEpubPath.empty()) {
    if (FsHelpers::hasXtcExtension(APP_STATE.openEpubPath)) {
      Xtc lastXtc(APP_STATE.openEpubPath, "/.crosspoint");
      if (lastXtc.load() && lastXtc.generateCoverBmp()) {
        coverBmpPath = lastXtc.getCoverBmpPath();
      }
    } else if (FsHelpers::hasTxtExtension(APP_STATE.openEpubPath)) {
      Txt lastTxt(APP_STATE.openEpubPath, "/.crosspoint");
      if (lastTxt.load() && lastTxt.generateCoverBmp()) {
        coverBmpPath = lastTxt.getCoverBmpPath();
      }
    } else if (FsHelpers::hasEpubExtension(APP_STATE.openEpubPath)) {
      Epub lastEpub(APP_STATE.openEpubPath, "/.crosspoint");
      if (lastEpub.load(true, true) && lastEpub.generateCoverBmp(cropped)) {
        coverBmpPath = lastEpub.getCoverBmpPath(cropped);
      }
    }
  }

  HalFile file;
  std::unique_ptr<Bitmap> bitmap = nullptr;
  if (!coverBmpPath.empty() && Storage.openFileForRead("SLP", coverBmpPath, file)) {
    auto bmp = makeUniqueNoThrow<Bitmap>(file);
    if (bmp && bmp->parseHeaders() == BmpReaderError::Ok) {
      bitmap = std::move(bmp);
    }
  }

  // 3. Define book geometry
  constexpr int W = 220;
  constexpr int H = 320;
  int cx = pageWidth / 2;
  int cy = pageHeight / 2 - 20;

  int x_l = cx - W / 2;
  int x_r = cx + W / 2;
  int y_t = cy - H / 2;
  int y_b = cy + H / 2;

  // Thickness based on estimatedLength
  int T = 15;
  if (info.success && info.estimatedLength > 0) {
    float lengthFactor = static_cast<float>(info.estimatedLength) / 1500000.0f;
    if (lengthFactor > 1.0f) lengthFactor = 1.0f;
    if (lengthFactor < 0.0f) lengthFactor = 0.0f;
    T = 15 + static_cast<int>(lengthFactor * 50);
  }

  float p = info.success ? info.progress : 0.0f;
  if (p < 0.0f) p = 0.0f;
  if (p > 1.0f) p = 1.0f;

  bool hasGreyscale = bitmap && bitmap->hasGreyscale() && 
                      (SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER);

  // 4. PASS 1: BW Render Pass
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.clearScreen(0xFF); // Clear screen to white

  // Draw the Right Pages Face
  int rightX[] = { x_r, x_r + T, x_r + T, x_r };
  int rightY[] = { y_t, y_t + T, y_b + T, y_b };
  renderer.fillPolygon(rightX, rightY, 4, false); // Fill with white
  renderer.drawLine(x_r, y_t, x_r + T, y_t + T, true);
  renderer.drawLine(x_r + T, y_t + T, x_r + T, y_b + T, true);
  renderer.drawLine(x_r + T, y_b + T, x_r, y_b, true);
  for (int y = y_t + 6; y < y_b; y += 6) {
    renderer.drawLine(x_r, y, x_r + T, y + T, true);
  }

  // Draw the Bottom Pages Face
  int bottomX[] = { x_l, x_r, x_r + T, x_l + T };
  int bottomY[] = { y_b, y_b, y_b + T, y_b + T };
  renderer.fillPolygon(bottomX, bottomY, 4, false); // Fill with white
  renderer.drawLine(x_l, y_b, x_l + T, y_b + T, true);
  renderer.drawLine(x_l + T, y_b + T, x_r + T, y_b + T, true);
  for (int offset = 6; offset < T; offset += 6) {
    renderer.drawLine(x_l + offset, y_b + offset, x_r + offset, y_b + offset, true);
  }

  // Draw book outlines
  renderer.drawRect(x_l, y_t, W, H, true);
  renderer.drawLine(x_r, y_t, x_r + T, y_t + T, true);
  renderer.drawLine(x_r + T, y_t + T, x_r + T, y_b + T, true);
  renderer.drawLine(x_r + T, y_b + T, x_l + T, y_b + T, true);
  renderer.drawLine(x_l + T, y_b + T, x_l, y_b, true);

  // Draw the bookmark ribbon
  int rx = cx + static_cast<int>(p * T);
  int ry = y_b + static_cast<int>(p * T);
  int r_end_y = y_b + T + 50;

  // Solid black ribbon
  renderer.fillRect(rx - 5, ry, 10, r_end_y - ry, true);
  // White borders
  renderer.drawLine(rx - 6, ry, rx - 6, r_end_y, false);
  renderer.drawLine(rx + 6, ry, rx + 6, r_end_y, false);
  // V-cut
  int xPoints[] = { rx - 6, rx, rx + 6 };
  int yPoints[] = { r_end_y, r_end_y - 6, r_end_y };
  renderer.fillPolygon(xPoints, yPoints, 3, false);

  // Draw book cover
  std::string title = "";
  std::string author = "";
  if (!APP_STATE.openEpubPath.empty()) {
    if (FsHelpers::hasXtcExtension(APP_STATE.openEpubPath)) {
      Xtc lastXtc(APP_STATE.openEpubPath, "/.crosspoint");
      if (lastXtc.load()) {
        title = lastXtc.getTitle();
        author = lastXtc.getAuthor();
      }
    } else if (FsHelpers::hasTxtExtension(APP_STATE.openEpubPath)) {
      Txt lastTxt(APP_STATE.openEpubPath, "/.crosspoint");
      if (lastTxt.load()) {
        title = lastTxt.getTitle();
      }
    } else if (FsHelpers::hasEpubExtension(APP_STATE.openEpubPath)) {
      Epub lastEpub(APP_STATE.openEpubPath, "/.crosspoint");
      if (lastEpub.load(true, true)) {
        title = lastEpub.getTitle();
        author = lastEpub.getAuthor();
      }
    }
  }
  drawBookCover(bitmap.get(), x_l, y_t, W, H, title, author);

  // Draw reading percentage
  if (info.success) {
    char progressStr[64];
    snprintf(progressStr, sizeof(progressStr), tr(STR_READ_PERCENT), static_cast<int>(p * 100.0f + 0.5f));
    renderer.drawCenteredText(UI_12_FONT_ID, y_b + T + 70, progressStr, true, EpdFontFamily::BOLD);
  } else {
    renderer.drawCenteredText(SMALL_FONT_ID, y_b + T + 70, tr(STR_SLEEPING));
  }

  // Invert screen if in dark mode
  if (SETTINGS.sleepScreen != CrossPointSettings::SLEEP_SCREEN_MODE::LIGHT) {
    renderer.invertScreen();
  }

  if (hasGreyscale) {
    renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);
  } else {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }

  if (hasGreyscale) {
    bitmap->rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    renderer.drawBitmap(*bitmap, x_l, y_t, W, H);
    renderer.copyGrayscaleLsbBuffers();

    bitmap->rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    renderer.drawBitmap(*bitmap, x_l, y_t, W, H);
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
  }
}

void SleepActivity::drawBookCover(const Bitmap* bitmap, int x, int y, int w, int h, const std::string& title, const std::string& author) const {
  if (bitmap) {
    renderer.drawBitmap(*bitmap, x, y, w, h);
  } else {
    renderer.fillRect(x, y, w, h, true);
    renderer.fillRect(x + 4, y + 4, w - 8, h - 8, false);
    renderer.drawRect(x + 8, y + 8, w - 16, h - 16, true);

    int titleY = y + 40;
    int lineH = renderer.getLineHeight(UI_12_FONT_ID);
    auto wrapped = renderer.wrappedText(UI_12_FONT_ID, title.c_str(), w - 32, 4, EpdFontFamily::BOLD);
    for (const auto& line : wrapped) {
      int textW = renderer.getTextWidth(UI_12_FONT_ID, line.c_str(), EpdFontFamily::BOLD);
      renderer.drawText(UI_12_FONT_ID, x + (w - textW) / 2, titleY, line.c_str(), true, EpdFontFamily::BOLD);
      titleY += lineH + 4;
    }

    if (!author.empty()) {
      int authorW = renderer.getTextWidth(UI_10_FONT_ID, author.c_str());
      renderer.drawText(UI_10_FONT_ID, x + (w - authorW) / 2, y + h - 50, author.c_str());
    }
  }
}
