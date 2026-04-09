#pragma once

#include <CrossPointSettings.h>
#include <GfxRenderer.h>
#include <Logging.h>

#include "MappedInputManager.h"

namespace ReaderUtils {

constexpr unsigned long GO_HOME_MS = 1000;
constexpr unsigned long LONGPRESS_ACTION_MS = 700;

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

inline bool isPageButtonHeld(const MappedInputManager& input) {
  return input.isPressed(MappedInputManager::Button::PageForward) ||
         input.isPressed(MappedInputManager::Button::PageBack) ||
         input.isPressed(MappedInputManager::Button::Left) || input.isPressed(MappedInputManager::Button::Right);
}

// Returns the new orientation index if a long-press rotation was detected, or -1.
inline int8_t detectRotation(const MappedInputManager& input) {
  if (SETTINGS.sideButtonLongPress != CrossPointSettings::LONGPRESS_ROTATE) return -1;
  if (input.getHeldTime() < LONGPRESS_ACTION_MS) return -1;
  const bool isNextHeld = input.isPressed(MappedInputManager::Button::PageForward) ||
                          input.isPressed(MappedInputManager::Button::Right);
  const bool isPrevHeld = input.isPressed(MappedInputManager::Button::PageBack) ||
                          input.isPressed(MappedInputManager::Button::Left);
  if (!isNextHeld && !isPrevHeld) return -1;
  const bool isBottom = (SETTINGS.sideButtonLayout == CrossPointSettings::PREV_NEXT) ? isNextHeld : isPrevHeld;
  return isBottom ? (SETTINGS.orientation + CrossPointSettings::ORIENTATION_COUNT - 1) %
                        CrossPointSettings::ORIENTATION_COUNT
                  : (SETTINGS.orientation + 1) % CrossPointSettings::ORIENTATION_COUNT;
}

struct PageTurnResult {
  bool prev;
  bool next;
};

inline PageTurnResult detectPageTurn(const MappedInputManager& input) {
  const bool usePress = (SETTINGS.sideButtonLongPress == CrossPointSettings::LONGPRESS_OFF);
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
