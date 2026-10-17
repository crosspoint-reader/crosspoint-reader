#include "BmpViewerActivity.h"

#include <Bitmap.h>
#include <Epub/converters/ImageDecoderFactory.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

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
      imageDitherMode(normalizeImageDitherModeValue(CrossPointSettings::IMAGE_DITHER_BAYER)) {
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
    renderError(tr(STR_UNSUPPORTED_IMAGE_FORMAT));
    return;
  }

  const bool rendered = renderCurrentImage();
  if (!rendered) {
    renderError(tr(STR_COULD_NOT_RENDER_IMAGE));
  }
}

void BmpViewerActivity::onExit() {
  Activity::onExit();
  ReaderUtils::enforceExitFullRefresh(renderer);
}

bool BmpViewerActivity::renderBmpImage(const bool showControls) {
  RenderLock lock(*this);
  FsFile file;
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));

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

  bmpHasGreyscale = bitmap.hasGreyscale();
  // Only render in grayscale when the bitmap actually carries greyscale data AND the user has it enabled.
  const bool renderGrayscale = bmpHasGreyscale && grayscaleDisplay;

  // Draw control hints. btn2 only shows the BW/Gray toggle when the bitmap supports greyscale —
  // pure 1-bit BMPs have nothing to toggle. The label shows the *target* mode (what pressing it switches to).
  const auto drawHints = [&]() {
    if (!showControls) return;
    const char* modeLabel =
        bmpHasGreyscale ? (grayscaleDisplay ? tr(STR_IMAGE_DISPLAY_BW) : tr(STR_IMAGE_DISPLAY_GRAYSCALE)) : "";
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), modeLabel, "", tr(STR_SET_SLEEP_SCREEN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  };

  renderer.setRenderMode(GfxRenderer::BW);
  renderer.clearScreen();
  renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0);
  drawHints();

  // Absolute planes follow crosspoint-reader PR #3469 (Bryan O'Sullivan / @bos);
  // the flow here is ours -- his viewer has no BW/grayscale toggle or dither
  // modes to preserve, and our themes need no grayscale-aware changes.
  //
  // Where the panel offers absolute planes, take them: on the X3 they run the
  // long XTH4 waveform in place of the short AA nudge, and it is the nudge that
  // shows this panel's 8-gate-line drive nonuniformity as vertical banding
  // across a dithered image. The pass pushes its own B/W base, so ask for it
  // here rather than displaying first; a panel that declines leaves us on the
  // ordinary push and the differential planes below.
  const bool panelHasAbsolute = renderGrayscale && renderer.supportsAbsoluteGrayPlanes();
  const bool absolutePass = panelHasAbsolute && renderer.beginAbsoluteGrayPass();
  if (!absolutePass) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }
  if (renderGrayscale) {
    // Which encoding a pass took is otherwise invisible from the outside: on the
    // UC8253 X3 both take the same time, so the waveform duration does not tell
    // them apart the way it does on the UC8279. Say it outright.
    LOG_DBG("BMP", "Grayscale planes: %s",
            absolutePass ? "absolute"
                         : (panelHasAbsolute ? "differential (panel declined the absolute pass)"
                                             : "differential (panel has no absolute encoding)"));

    // Multi-pass 4-level grayscale render — mirrors SleepActivity::renderBitmapSleepScreen.
    // A differential plane starts empty and lets the B/W base supply black,
    // white and the hints. An absolute plane carries the whole frame instead:
    // a clear bit no longer means "leave as is", so it starts white and has to
    // take the hints as well, or they would settle grey over a white page.
    const auto renderPlane = [&](const GfxRenderer::RenderMode mode) {
      bitmap.rewindToData();
      renderer.clearScreen(absolutePass ? 0xFF : 0x00);
      renderer.setRenderMode(mode);
      renderer.setAbsoluteGrayPlanes(absolutePass);
      renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0);
      if (absolutePass) {
        // White is 11 and black 00 across the two planes, so ordinary BW
        // drawing puts a 1-bit overlay into a plane correctly. Returning to BW
        // also clears the absolute flag, hence re-arming it per plane above.
        renderer.setRenderMode(GfxRenderer::BW);
        drawHints();
      }
    };

    renderPlane(GfxRenderer::GRAYSCALE_LSB);
    renderer.copyGrayscaleLsbBuffers();

    renderPlane(GfxRenderer::GRAYSCALE_MSB);
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
  }

  file.close();
  return true;
}

bool BmpViewerActivity::renderDecodedImage(const bool showControls) {
  RenderLock lock(*this);
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));

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

  // Helper to draw the on-screen control hints. The btn3 label shows the *other* mode
  // (i.e. what pressing it would switch to).
  const auto drawHints = [&]() {
    if (!showControls) return;
    const char* modeLabel = grayscaleDisplay ? tr(STR_IMAGE_DISPLAY_BW) : tr(STR_IMAGE_DISPLAY_GRAYSCALE);
#ifdef ENABLE_IMAGE_DITHERING_EXTENSION
    const char* btn3Label = grayscaleDisplay ? I18N.get(getCurrentDitherModeLabel()) : "";
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), modeLabel, btn3Label, tr(STR_SET_SLEEP_SCREEN));
#else
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), modeLabel, "", tr(STR_SET_SLEEP_SCREEN));
#endif
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  };

  if (!grayscaleDisplay) {
    // Pure black-and-white path: single decode with 1-bit Atkinson dither.
    config.monochromeOutput = true;
    renderer.setRenderMode(GfxRenderer::BW);
    renderer.clearScreen();
    if (!decoder->decodeToFramebuffer(filePath, renderer, config)) {
      return false;
    }
    drawHints();
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return true;
  }

  // Grayscale path: three decode passes (BW + LSB + MSB), 4-level dither in all of them.
  // Mirrors SleepActivity::renderCustomSleepScreen so the BW plane carries the same
  // 4-level quantization as the gray planes layered on top.
  config.monochromeOutput = false;

  renderer.setRenderMode(GfxRenderer::BW);
  renderer.clearScreen();
  if (!decoder->decodeToFramebuffer(filePath, renderer, config)) {
    return false;
  }
  drawHints();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  if (!decoder->decodeToFramebuffer(filePath, renderer, config)) {
    renderer.setRenderMode(GfxRenderer::BW);
    return false;
  }
  renderer.copyGrayscaleLsbBuffers();

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  if (!decoder->decodeToFramebuffer(filePath, renderer, config)) {
    renderer.setRenderMode(GfxRenderer::BW);
    return false;
  }
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);
  return true;
}

void BmpViewerActivity::toggleDisplayMode() {
  grayscaleDisplay = !grayscaleDisplay;
  // Switching between 1-bit BW and 4-level grayscale requires a full refresh to clear
  // ghosting from the previous mode — a half refresh leaves visible residue.
  {
    RenderLock lock(*this);
    renderer.clearScreen();
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  }
  if (!renderCurrentImage()) {
    renderError(tr(STR_COULD_NOT_RENDER_IMAGE));
  }
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

  if (!renderCurrentImage()) {
    renderError(tr(STR_COULD_NOT_RENDER_IMAGE));
  }
}
#endif

void BmpViewerActivity::renderError(const char* message) {
  RenderLock lock(*this);
  const auto pageHeight = renderer.getScreenHeight();
  renderer.clearScreen();
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, message);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

bool BmpViewerActivity::setBmpFileAsSleepScreen(const std::string& filePath) {
  const bool success = (filePath == SLEEP_BMP_PATH) ? true : Storage.copyFile("BMP", filePath, SLEEP_BMP_PATH);
  if (success) {
    SETTINGS.sleepScreen = CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM;
    SETTINGS.saveToFile();
    LOG_INF("BMP", "Set %s as sleep screen", filePath.c_str());
  } else {
    LOG_ERR("BMP", "Failed to copy %s as sleep screen", filePath.c_str());
  }
  return success;
}

ConfirmationActivity::Question BmpViewerActivity::sleepScreenQuestion(const std::string& imagePath) {
  ConfirmationActivity::Question question;
  question.headline = tr(STR_SET_SLEEP_SCREEN_QUESTION);
  question.message = imagePath.substr(imagePath.find_last_of('/') + 1);
  question.acceptLabel = StrId::STR_SET_SLEEP_SCREEN;
  return question;
}

// One press used to replace /sleep.bmp, and replaceSleepBmpFromTemp() drops the backup once the
// new image is in, so a stray press lost the previous sleep image for good.
// Ported from crosspoint-reader commit 6d649b66f (PR #3994, Uri Tauber).
void BmpViewerActivity::confirmSetAsSleepScreen() {
  auto confirmation = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, sleepScreenQuestion(filePath));
  if (!confirmation) {
    LOG_ERR("BMP", "OOM: sleep screen confirmation (%zu bytes)", sizeof(ConfirmationActivity));
    return;
  }
  startActivityForResult(std::move(confirmation), [this](const ActivityResult& result) {
    // The question was drawn over the image. A decoded image is drawn again by setAsSleepScreen(),
    // which renders it for the capture; a BMP is drawn here, so that the outcome's popup (drawn over
    // the frame on the panel) lands on the image.
    if (result.isCancelled || isBmpFile(filePath)) {
      if (!renderCurrentImage()) {
        renderError(tr(STR_COULD_NOT_RENDER_IMAGE));
        return;
      }
    }
    if (!result.isCancelled) setAsSleepScreen();
  });
}

void BmpViewerActivity::setAsSleepScreen() {
  bool success = false;

  if (FsHelpers::hasBmpExtension(filePath)) {
    success = setBmpFileAsSleepScreen(filePath);
  } else {
    const bool renderedForCapture = renderDecodedImage(false);
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

    if (success) {
      SETTINGS.sleepScreen = CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM;
      SETTINGS.saveToFile();
      LOG_INF("BMP", "Set %s as sleep screen", filePath.c_str());
    }
  }

  if (!success) {
    LOG_ERR("BMP", "Failed to set %s as sleep screen", filePath.c_str());
    RenderLock lock(*this);
    // drawPopup ships the frame; preseed its refresh mode to HALF instead of shipping twice.
    renderer.setNextDisplayRefreshMode(HalDisplay::HALF_REFRESH);
    GUI.drawPopup(renderer, tr(STR_FAILED_TO_SET_SLEEP_SCREEN));
    return;
  }

  RenderLock lock(*this);
  renderer.setNextDisplayRefreshMode(HalDisplay::HALF_REFRESH);
  GUI.drawPopup(renderer, tr(STR_SLEEP_SCREEN_SET));
}

void BmpViewerActivity::loop() {
  // Keep CPU awake/polling so 1st click works
  Activity::loop();

  const bool toggleSupported = isBmpFile(filePath) ? bmpHasGreyscale : true;
  ButtonEventManager::ButtonEvent ev;
  while (buttonEvents.consumeEvent(ev)) {
    if (ev.button == MappedInputManager::Button::Back) {
      if (ev.type == ButtonEventManager::PressType::Long) {
        onGoHome();
        return;
      }
      if (ev.type == ButtonEventManager::PressType::Short) {
        finish();
        return;
      }
    }

    if (ev.button == MappedInputManager::Button::Confirm && ev.type == ButtonEventManager::PressType::Short) {
      if (toggleSupported) {
        toggleDisplayMode();
      }
      return;
    }

#ifdef ENABLE_IMAGE_DITHERING_EXTENSION
    if (ev.button == MappedInputManager::frontStripPrevious() && ev.type == ButtonEventManager::PressType::Short &&
        !isBmpFile(filePath) && grayscaleDisplay) {
      cycleDitherMode();
      return;
    }
#endif

    // The viewer's two commands are labelled on the front strip and stay there; only which of the
    // two is drawn first changes with orientation, and frontStripPrevious/Next answer for that
    // exactly as mapLabels() does below.
    if (ev.button == MappedInputManager::frontStripNext() && ev.type == ButtonEventManager::PressType::Short) {
      confirmSetAsSleepScreen();
      return;
    }
  }
}