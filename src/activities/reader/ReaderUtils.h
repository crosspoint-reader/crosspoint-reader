#pragma once

#include <CrossPointSettings.h>
#include <GfxRenderer.h>
#include <HalTiltSensor.h>
#include <Logging.h>
#include <Memory.h>

#include "MappedInputManager.h"

namespace ReaderUtils {

constexpr unsigned long GO_HOME_MS = 1000;
constexpr unsigned long SKIP_HOLD_MS = 700;
constexpr unsigned long BOOKMARK_HOLD_MS = 400;
constexpr unsigned long BOOKMARK_MESSAGE_DURATION_MS = 2500;
constexpr unsigned long DICTIONARY_MESSAGE_DURATION_MS = 1500;

inline void applyOrientation(GfxRenderer& renderer, const uint8_t orientation) {
  switch (orientation) {
    case CrossPointSettings::ORIENTATION::PORTRAIT:
      renderer.setOrientation(GfxRenderer::Orientation::Portrait);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeClockwise);
      break;
    case CrossPointSettings::ORIENTATION::INVERTED:
      renderer.setOrientation(GfxRenderer::Orientation::PortraitInverted);
      break;
    case CrossPointSettings::ORIENTATION::LANDSCAPE_CCW:
      renderer.setOrientation(GfxRenderer::Orientation::LandscapeCounterClockwise);
      break;
    default:
      break;
  }
}

struct PageTurnResult {
  bool prev;
  bool next;
  bool fromTilt;
};

inline PageTurnResult detectPageTurn(const MappedInputManager& input) {
  const bool usePress = SETTINGS.longPressButtonBehavior == SETTINGS.OFF;
  const bool tiltNext = SETTINGS.tiltPageTurn && halTiltSensor.wasTiltedForward();
  const bool tiltPrev = SETTINGS.tiltPageTurn && halTiltSensor.wasTiltedBack();
  const bool swapFront = input.isNavDirectionSwapped();
  const auto prevButton = swapFront ? MappedInputManager::Button::Right : MappedInputManager::Button::Left;
  const auto nextButton = swapFront ? MappedInputManager::Button::Left : MappedInputManager::Button::Right;
  const bool prev =
      tiltPrev ||
      (usePress ? (input.wasPressed(MappedInputManager::Button::PageBack) || input.wasPressed(prevButton))
                : (input.wasReleased(MappedInputManager::Button::PageBack) || input.wasReleased(prevButton)));
  const bool powerTurn = SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN &&
                         input.wasReleased(MappedInputManager::Button::Power);
  const bool next = tiltNext || (usePress ? (input.wasPressed(MappedInputManager::Button::PageForward) || powerTurn ||
                                             input.wasPressed(nextButton))
                                          : (input.wasReleased(MappedInputManager::Button::PageForward) || powerTurn ||
                                             input.wasReleased(nextButton)));
  return {prev, next, tiltPrev || tiltNext};
}

inline bool shortPowerButtonActionTriggered(const MappedInputManager& input, CrossPointSettings::SHORT_PWRBTN action) {
  return SETTINGS.shortPwrBtn == action && input.wasReleased(MappedInputManager::Button::Power) &&
         !input.wasReleased(MappedInputManager::Button::Down);
}

inline uint8_t rotatedOrientationForNavigation(bool nextTriggered) {
  return nextTriggered ? (SETTINGS.orientation - 1 + SETTINGS.ORIENTATION_COUNT) % SETTINGS.ORIENTATION_COUNT
                       : (SETTINGS.orientation + 1) % SETTINGS.ORIENTATION_COUNT;
}

inline void displayWithRefreshCycle(const GfxRenderer& renderer, int& pagesUntilFullRefresh) {
  if (pagesUntilFullRefresh <= 1) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    renderer.displayBuffer();
    pagesUntilFullRefresh--;
  }
}

// Grayscale anti-aliasing pass. Renders content twice (LSB + MSB) to build
// the grayscale buffer. Only the content callback is re-rendered — status bars
// and other overlays should be drawn before calling this.
// Kept as a template to avoid std::function overhead; instantiated once per reader type.
template <typename RenderFn>
void renderAntiAliased(GfxRenderer& renderer, RenderFn&& renderFn) {
  if (renderer.supportsStripGrayscale()) {
    constexpr int STRIP_ROWS = 80;
    const int displayHeight = renderer.getDisplayHeight();
    const int displayWidthBytes = renderer.getDisplayWidthBytes();
    auto scratch = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(displayWidthBytes) * STRIP_ROWS);
    if (!scratch) {
      LOG_ERR("READER", "OOM: grayscale strip scratch (%d bytes); skipping anti-aliasing",
              displayWidthBytes * STRIP_ROWS);
      return;
    }

    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    for (int y = 0; y < displayHeight; y += STRIP_ROWS) {
      const int rows = (displayHeight - y < STRIP_ROWS) ? (displayHeight - y) : STRIP_ROWS;
      renderer.beginStripTarget(scratch.get(), y, rows);
      renderer.clearScreen(0x00);
      renderFn();
      renderer.endStripTarget();
      renderer.writeGrayscalePlaneStrip(true, scratch.get(), y, rows);
    }

    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    for (int y = 0; y < displayHeight; y += STRIP_ROWS) {
      const int rows = (displayHeight - y < STRIP_ROWS) ? (displayHeight - y) : STRIP_ROWS;
      renderer.beginStripTarget(scratch.get(), y, rows);
      renderer.clearScreen(0x00);
      renderFn();
      renderer.endStripTarget();
      renderer.writeGrayscalePlaneStrip(false, scratch.get(), y, rows);
    }

    renderer.setRenderMode(GfxRenderer::BW);
    renderer.displayGrayBuffer();
    renderer.cleanupGrayscaleWithFrameBuffer();
    return;
  }

  if (!renderer.storeBwBuffer()) {
    LOG_ERR("READER", "Failed to store BW buffer for anti-aliasing");
    return;
  }

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  renderFn();
  renderer.copyGrayscaleLsbBuffers();

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  renderFn();
  renderer.copyGrayscaleMsbBuffers();

  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);

  renderer.restoreBwBuffer();
}

}  // namespace ReaderUtils
