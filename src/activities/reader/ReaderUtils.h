#pragma once

#include <CrossPointSettings.h>
#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <HalTiltSensor.h>
#include <Logging.h>

#include "DeviceProfile.h"
#include "MappedInputManager.h"

namespace ReaderUtils {

constexpr unsigned long GO_HOME_MS = 1000;
constexpr unsigned long SKIP_HOLD_MS = 700;

// True iff the user enabled AA AND the current device's panel can actually
// produce usable grayscale. On devices like Murphy (UC8253 with asymmetric
// drive rails) the AA pipeline produces inverted/flashing artefacts, so we
// treat AA as disabled even if the user setting is on.
inline bool effectiveAntiAlias() {
  return SETTINGS.textAntiAliasing && DeviceProfiles::current().supportsGrayscaleAntiAlias;
}

inline int touchWidth() {
  return BoardConfig::ACTIVE.displayWidth > BoardConfig::ACTIVE.displayHeight ? BoardConfig::ACTIVE.displayWidth
                                                                              : BoardConfig::ACTIVE.displayHeight;
}

inline int touchHeight() {
  return BoardConfig::ACTIVE.displayWidth > BoardConfig::ACTIVE.displayHeight ? BoardConfig::ACTIVE.displayHeight
                                                                              : BoardConfig::ACTIVE.displayWidth;
}

inline bool consumeTopLeftTouchBack(const MappedInputManager& input) {
  static unsigned long lastConsumedTouchBackAt = 0;
  const auto touchPoint = input.getTouchPoint();
  if (!touchPoint.valid || touchPoint.timestamp == lastConsumedTouchBackAt || millis() - touchPoint.timestamp >= 1500) {
    return false;
  }

  if (touchPoint.x >= touchWidth() / 3 || touchPoint.y >= touchHeight() / 3) {
    return false;
  }

  lastConsumedTouchBackAt = touchPoint.timestamp;
  LOG_DBG("TOUCH", "reader top-left back x=%u y=%u", touchPoint.x, touchPoint.y);
  return true;
}

inline bool consumeCenterTouchHold(const MappedInputManager& input, const unsigned long holdMs = 450) {
  static unsigned long lastConsumedCenterHoldAt = 0;
  const auto touchPoint = input.getTouchPoint();
  if (!touchPoint.valid || touchPoint.timestamp == lastConsumedCenterHoldAt || !input.isTouchPressed() ||
      millis() - touchPoint.timestamp < holdMs) {
    return false;
  }

  const int width = touchWidth();
  const int height = touchHeight();
  if (touchPoint.x < width / 3 || touchPoint.x >= (width * 2) / 3 || touchPoint.y < height / 3 ||
      touchPoint.y >= (height * 2) / 3) {
    return false;
  }

  lastConsumedCenterHoldAt = touchPoint.timestamp;
  LOG_DBG("TOUCH", "reader center hold x=%u y=%u", touchPoint.x, touchPoint.y);
  return true;
}

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
  const bool swapFront =
      SETTINGS.frontButtonFollowOrientation && (SETTINGS.orientation == CrossPointSettings::INVERTED ||
                                                SETTINGS.orientation == CrossPointSettings::LANDSCAPE_CCW);
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
  // Devices whose panel can't sustain 4-level grayscale (Murphy's UC8253
  // with asymmetric drive rails — see Murphy_M3 findings) skip the AA
  // pass entirely. The B/W base frame the caller already produced is
  // already on the panel; rendering an AA layer over it with our gray
  // pipeline produces inverted/flashing artefacts. Returning here leaves
  // the panel showing the clean B/W render.
  if (!DeviceProfiles::current().supportsGrayscaleAntiAlias) {
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
