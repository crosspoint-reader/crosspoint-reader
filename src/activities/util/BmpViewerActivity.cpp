#include "BmpViewerActivity.h"

#include <Bitmap.h>
#include <Epub/converters/ImageDecoderFactory.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <cmath>

#include "../reader/ReaderUtils.h"
#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ScreenshotUtil.h"

namespace {
constexpr const char* SLEEP_BMP_PATH = "/sleep.bmp";
constexpr const char* SLEEP_BMP_TMP_PATH = "/sleep.bmp.tmp";
constexpr const char* SLEEP_BMP_BACKUP_PATH = "/sleep.bmp.bak";

#ifdef ENABLE_IMAGE_DITHERING_EXTENSION
uint8_t normalizeImageDitherModeValue(uint8_t mode) { return static_cast<uint8_t>(imageDitherModeFromSetting(mode)); }
#endif

bool isBmpFile(const std::string& path) { return FsHelpers::hasBmpExtension(path); }

bool isSupportedImageFile(const std::string& path) {
  return FsHelpers::hasBmpExtension(path) || FsHelpers::hasJpgExtension(path) || FsHelpers::hasPngExtension(path);
}

void computeCenteredImagePlacement(const int imageWidth, const int imageHeight, const int pageWidth,
                                   const int pageHeight, int& x, int& y, int& renderWidth, int& renderHeight) {
  renderWidth = imageWidth;
  renderHeight = imageHeight;

  if (imageWidth > pageWidth || imageHeight > pageHeight) {
    const float ratio = static_cast<float>(imageWidth) / static_cast<float>(imageHeight);
    const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

    if (ratio > screenRatio) {
      renderWidth = pageWidth;
      renderHeight = std::max(1, static_cast<int>(std::round(static_cast<float>(pageWidth) / ratio)));
    } else {
      renderHeight = pageHeight;
      renderWidth = std::max(1, static_cast<int>(std::round(static_cast<float>(pageHeight) * ratio)));
    }

    x = std::max(0, (pageWidth - renderWidth) / 2);
    y = std::max(0, (pageHeight - renderHeight) / 2);
    return;
  }

  renderWidth = std::max(1, renderWidth);
  renderHeight = std::max(1, renderHeight);
  x = std::max(0, (pageWidth - renderWidth) / 2);
  y = std::max(0, (pageHeight - renderHeight) / 2);
}

bool replaceSleepBmpFromTemp() {
  const bool hadExistingTarget = Storage.exists(SLEEP_BMP_PATH);
  bool movedExistingToBackup = false;

  if (Storage.exists(SLEEP_BMP_BACKUP_PATH)) {
    Storage.remove(SLEEP_BMP_BACKUP_PATH);
  }

  if (hadExistingTarget) {
    movedExistingToBackup = Storage.rename(SLEEP_BMP_PATH, SLEEP_BMP_BACKUP_PATH);
    if (!movedExistingToBackup) {
      return false;
    }
  }

  if (Storage.rename(SLEEP_BMP_TMP_PATH, SLEEP_BMP_PATH)) {
    if (movedExistingToBackup) {
      Storage.remove(SLEEP_BMP_BACKUP_PATH);
    }
    return true;
  }

  if (movedExistingToBackup) {
    Storage.rename(SLEEP_BMP_BACKUP_PATH, SLEEP_BMP_PATH);
  }
  return false;
}
}  // namespace

BmpViewerActivity::BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path)
    : Activity("BmpViewer", renderer, mappedInput),
      filePath(std::move(path))
#ifdef ENABLE_IMAGE_DITHERING_EXTENSION
      ,
      imageDitherMode(normalizeImageDitherModeValue(SETTINGS.imageDithering)),
      initialImageDitherMode(imageDitherMode),
      imageDitherSettingsDirty(false) {
}
#else
{
}
#endif

bool BmpViewerActivity::renderCurrentImage(const bool showControls) {
  return isBmpFile(filePath) ? renderBmpImage(showControls) : renderDecodedImage(showControls);
}

void BmpViewerActivity::onEnter() {
  Activity::onEnter();
  if (!isSupportedImageFile(filePath)) {
    renderError("Unsupported image format");
    return;
  }

  const bool rendered = renderCurrentImage();
  if (!rendered) {
    renderError("Could not render image");
  }
}

void BmpViewerActivity::onExit() {
#ifdef ENABLE_IMAGE_DITHERING_EXTENSION
  saveDitherSettingsIfNeeded();
#endif
  Activity::onExit();
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
}

bool BmpViewerActivity::renderBmpImage(const bool showControls) {
  FsFile file;
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  Rect popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  GUI.fillPopupProgress(renderer, popupRect, 20);

  if (!Storage.openFileForRead("BMP", filePath, file)) {
    return false;
  }

  Bitmap bitmap(file, true);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) {
    file.close();
    return false;
  }

  int x, y, renderWidth, renderHeight;
  computeCenteredImagePlacement(bitmap.getWidth(), bitmap.getHeight(), pageWidth, pageHeight, x, y, renderWidth,
                                renderHeight);

  GUI.fillPopupProgress(renderer, popupRect, 50);

  renderer.clearScreen();
  renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0);
  if (showControls) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", tr(STR_SET_SLEEP_SCREEN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);

  file.close();
  return true;
}

bool BmpViewerActivity::renderDecodedImage(const bool showControls) {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  Rect popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  GUI.fillPopupProgress(renderer, popupRect, 20);

  ImageToFramebufferDecoder* decoder = ImageDecoderFactory::getDecoder(filePath);
  if (!decoder) {
    return false;
  }

  ImageDimensions dims{};
  if (!decoder->getDimensions(filePath, dims) || dims.width <= 0 || dims.height <= 0) {
    return false;
  }

  int x, y, renderWidth, renderHeight;
  computeCenteredImagePlacement(dims.width, dims.height, pageWidth, pageHeight, x, y, renderWidth, renderHeight);

  GUI.fillPopupProgress(renderer, popupRect, 50);
  renderer.clearScreen();

  RenderConfig config{};
  config.x = x;
  config.y = y;
  config.maxWidth = renderWidth;
  config.maxHeight = renderHeight;
  config.useExactDimensions = true;
  config.useGrayscale = true;
  config.useDithering = true;
#ifdef ENABLE_IMAGE_DITHERING_EXTENSION
  config.ditherMode = imageDitherModeFromSetting(imageDitherMode);
#else
  config.ditherMode = ImageDitherMode::Bayer;
#endif

  if (!decoder->decodeToFramebuffer(filePath, renderer, config)) {
    return false;
  }

  if (showControls) {
#ifdef ENABLE_IMAGE_DITHERING_EXTENSION
    const auto labels =
        mappedInput.mapLabels(tr(STR_BACK), "", I18N.get(getCurrentDitherModeLabel()), tr(STR_SET_SLEEP_SCREEN));
#else
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", tr(STR_SET_SLEEP_SCREEN));
#endif
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  return true;
}

#ifdef ENABLE_IMAGE_DITHERING_EXTENSION
StrId BmpViewerActivity::getCurrentDitherModeLabel() const {
  switch (imageDitherModeFromSetting(imageDitherMode)) {
    case ImageDitherMode::Atkinson:
      return StrId::STR_IMAGE_DITHER_ATKINSON;
    case ImageDitherMode::DiffusedBayer:
      return StrId::STR_IMAGE_DITHER_DIFFUSED_BAYER;
    case ImageDitherMode::Bayer:
    case ImageDitherMode::COUNT:
    default:
      return StrId::STR_IMAGE_DITHER_BAYER;
  }
}

void BmpViewerActivity::cycleDitherMode() {
  imageDitherMode = (imageDitherMode + 1) % CrossPointSettings::IMAGE_DITHERING_COUNT;
  SETTINGS.imageDithering = imageDitherMode;
  imageDitherSettingsDirty = (imageDitherMode != initialImageDitherMode);

  if (!renderCurrentImage()) {
    renderError("Could not render image");
  }
}

void BmpViewerActivity::saveDitherSettingsIfNeeded() {
  if (!imageDitherSettingsDirty) {
    return;
  }

  SETTINGS.imageDithering = imageDitherMode;
  SETTINGS.saveToFile();
  initialImageDitherMode = imageDitherMode;
  imageDitherSettingsDirty = false;
}
#endif

void BmpViewerActivity::renderError(const char* message) {
  const auto pageHeight = renderer.getScreenHeight();
  renderer.clearScreen();
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, message);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void BmpViewerActivity::setAsSleepScreen() {
  bool success = false;

  if (FsHelpers::hasBmpExtension(filePath)) {
    success = (filePath == SLEEP_BMP_PATH) ? true : Storage.copyFile("BMP", filePath, SLEEP_BMP_PATH);
  } else {
    const bool renderedForCapture = isBmpFile(filePath) ? renderBmpImage(false) : renderDecodedImage(false);
    if (renderedForCapture) {
      Storage.remove(SLEEP_BMP_TMP_PATH);
      if (ScreenshotUtil::saveFramebufferAsBmp(SLEEP_BMP_TMP_PATH, renderer.getFrameBuffer(), display.getDisplayWidth(),
                                               display.getDisplayHeight())) {
        success = replaceSleepBmpFromTemp();
      }
    }

    if (!success && Storage.exists(SLEEP_BMP_TMP_PATH)) {
      Storage.remove(SLEEP_BMP_TMP_PATH);
    }
  }

  if (!success) {
    LOG_ERR("BMP", "Failed to set %s as sleep screen", filePath.c_str());
    GUI.drawPopup(renderer, "Failed to set sleep screen");
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return;
  }

  SETTINGS.sleepScreen = CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM;
  SETTINGS.saveToFile();
  LOG_INF("BMP", "Set %s as sleep screen", filePath.c_str());

  GUI.drawPopup(renderer, tr(STR_SLEEP_SCREEN_SET));
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void BmpViewerActivity::loop() {
  // Keep CPU awake/polling so 1st click works
  Activity::loop();

  // Long press BACK (1s+) goes to home screen
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    onGoHome();
    return;
  }

  // Short press BACK returns to the calling activity
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    finish();
    return;
  }

#ifdef ENABLE_IMAGE_DITHERING_EXTENSION
  if (!isBmpFile(filePath) && mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    cycleDitherMode();
    return;
  }
#endif

  // Next/Right button: set this image as the sleep screen
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    setAsSleepScreen();
    return;
  }
}