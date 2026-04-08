#pragma once

#include <CrossPointSettings.h>
#include <GfxRenderer.h>
#include <Logging.h>

#include "MappedInputManager.h"

namespace ReaderUtils {

constexpr unsigned long GO_HOME_MS = 1000;

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
// hold used to wake the device leaks into detectPageTurn() and triggers a page turn or, with
// longPressChapterSkip enabled, a chapter skip (the wake-hold easily exceeds skipChapterMs).
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
  const bool usePress = !SETTINGS.longPressChapterSkip;
  const bool prev = usePress ? (input.wasPressed(MappedInputManager::Button::PageBack) ||
                                input.wasPressed(MappedInputManager::Button::Left))
                             : (input.wasReleased(MappedInputManager::Button::PageBack) ||
                                input.wasReleased(MappedInputManager::Button::Left));
  const bool powerTurn = SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN &&
                         input.wasReleased(MappedInputManager::Button::Power);
  const bool next = usePress ? (input.wasPressed(MappedInputManager::Button::PageForward) || powerTurn ||
                                input.wasPressed(MappedInputManager::Button::Right))
                             : (input.wasReleased(MappedInputManager::Button::PageForward) || powerTurn ||
                                input.wasReleased(MappedInputManager::Button::Right));
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
  // Schedule the next displayed screen to use a full refresh, rather than refreshing
  // the current reader screen as it closes.
  renderer.setNextDisplayRefreshMode(HalDisplay::FULL_REFRESH);
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
