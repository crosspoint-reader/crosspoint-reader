#pragma once

#include <CrossPointSettings.h>
#include <GfxRenderer.h>
#include <HalTiltSensor.h>
#include <Logging.h>

#include <cstdint>

#include "ButtonEventManager.h"
#include "MappedInputManager.h"

namespace ReaderUtils {

constexpr unsigned long GO_HOME_MS = 1000;

// Round-half-up integer division clamped to [0, 100], used as the percent byte appended to
// progress.bin so the home screen can render a per-book badge without re-loading the document.
// All reader types must funnel through this so the displayed value matches across formats.
inline uint8_t pageProgressPercentByte(int currentPage, int totalPages) {
  if (totalPages <= 0 || currentPage < 0) {
    return 0;
  }
  const long numerator = static_cast<long>(currentPage + 1) * 200L + totalPages;
  const long percent = numerator / (2L * totalPages);
  if (percent < 0) return 0;
  if (percent > 100) return 100;
  return static_cast<uint8_t>(percent);
}

// Round-half-up clamp for a pre-computed [0,1] progress fraction (used by EPUB, where progress
// is byte-weighted across spine items rather than a simple page ratio).
inline uint8_t fractionProgressPercentByte(float fraction) {
  const int percent = static_cast<int>(fraction * 100.0f + 0.5f);
  if (percent < 0) return 0;
  if (percent > 100) return 100;
  return static_cast<uint8_t>(percent);
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

// Suppresses input processing on activity entry until the user has released all buttons and a
// clean frame (no pending press/release events) has been observed. Without this, the power-button
// hold used to wake the device leaks into detectPageTurn() and triggers a page turn or chapter
// skip (the wake-hold easily exceeds skipChapterMs).
// Each reader holds an instance, calls arm() in onEnter(), and calls shouldDrain() at the top
// of loop() — returning early when it returns true.
struct InputDrainGuard {
  bool active = false;

  void arm() { active = true; }

  bool shouldDrain(const MappedInputManager& input) {
    if (!active) {
      return false;
    }
    using B = MappedInputManager::Button;
    const bool anyHeld = input.isPressed(B::Back) || input.isPressed(B::Confirm) || input.isPressed(B::Left) ||
                         input.isPressed(B::Right) || input.isPressed(B::Up) || input.isPressed(B::Down) ||
                         input.isPressed(B::Power) || input.isPressed(B::PageBack) || input.isPressed(B::PageForward);
    if (anyHeld || input.wasAnyPressed() || input.wasAnyReleased()) {
      return true;
    }
    active = false;
    return false;
  }
};

struct PageTurnResult {
  bool prev;
  bool next;
};

inline PageTurnResult detectPageTurn(const MappedInputManager& input) {
  // Only treat wasReleased as a page turn when the button's short-press action is default.
  // Non-default short-press actions are dispatched by the global dispatcher in main.cpp;
  // counting wasReleased as well would double-fire the action.
  // Also suppress immediate page-turns if a double-click action is configured for the button,
  // because the button event system delays short events until the double-click window expires.
  using BA = CrossPointSettings::BUTTON_ACTION;
  using TA = CrossPointSettings::TILT_GESTURE_ACTION;
  const bool tiltNegative = SETTINGS.tiltPageTurn && halTiltSensor.wasTiltedBack();
  const bool tiltPositive = SETTINGS.tiltPageTurn && halTiltSensor.wasTiltedForward();
  bool tiltPrev = false;
  bool tiltNext = false;
  auto applyTiltAction = [&](uint8_t action) {
    if (action == TA::TILT_ACT_NEXT_PAGE) {
      tiltNext = true;
    } else if (action == TA::TILT_ACT_PREV_PAGE) {
      tiltPrev = true;
    }
  };
  if (tiltPositive) {
    applyTiltAction(SETTINGS.tiltPositiveAction);
  }
  if (tiltNegative) {
    applyTiltAction(SETTINGS.tiltNegativeAction);
  }
  const bool prevButtonReleased = (SETTINGS.btnShortPageBack == BA::BTN_DEFAULT &&
                                   !globalButtonEvents().hasDoubleAction(MappedInputManager::Button::PageBack) &&
                                   input.wasReleased(MappedInputManager::Button::PageBack)) ||
                                  (SETTINGS.btnShortLeft == BA::BTN_DEFAULT &&
                                   !globalButtonEvents().hasDoubleAction(MappedInputManager::Button::Left) &&
                                   input.wasReleased(MappedInputManager::Button::Left));
  const bool nextButtonReleased = (SETTINGS.btnShortPageForward == BA::BTN_DEFAULT &&
                                   !globalButtonEvents().hasDoubleAction(MappedInputManager::Button::PageForward) &&
                                   input.wasReleased(MappedInputManager::Button::PageForward)) ||
                                  (SETTINGS.btnShortRight == BA::BTN_DEFAULT &&
                                   !globalButtonEvents().hasDoubleAction(MappedInputManager::Button::Right) &&
                                   input.wasReleased(MappedInputManager::Button::Right));

  const bool prev = tiltPrev || prevButtonReleased;
  const bool next = tiltNext || nextButtonReleased;
  return {prev, next};
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

inline void enforceExitFullRefresh(const GfxRenderer& renderer) {
  // Reader exits can leave visible ghosting when the next screen is rendered with a fast LUT.
  // Schedule the next displayed screen to use a half refresh, rather than refreshing
  // the current reader screen as it closes.
  renderer.setNextDisplayRefreshMode(HalDisplay::HALF_REFRESH);
}

// Grayscale anti-aliasing pass. Renders content twice (LSB + MSB) to build
// the grayscale buffer. Only the content callback is re-rendered — status bars
// and other overlays should be drawn before calling this.
// Kept as a template to avoid std::function overhead; instantiated once per reader type.
template <typename RenderFn>
void renderAntiAliased(GfxRenderer& renderer, RenderFn&& renderFn) {
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
