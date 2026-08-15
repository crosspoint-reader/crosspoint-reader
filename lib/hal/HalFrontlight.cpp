#include "HalFrontlight.h"

#include <Logging.h>

HalFrontlight HalFrontlight::instance;

namespace {
// Night Light ladder: SDK perceptual levels whose quadratic curve lands on
// duties {8, 6, 4, 2, 1} at a 10-bit PWM (the X4 Pro profile) — i.e. about
// 0.8 / 0.6 / 0.4 / 0.2 / 0.1 % of full, the spread validated on hardware.
// Level 1 is the smallest non-zero duty on any LEDC board, so the ladder stays
// monotonic at other PWM resolutions even though the exact percentages shift.
// Not valid for the PM1 PMIC path — see supportsNightLight().
constexpr uint8_t DIM_LEVELS[HalFrontlight::DIM_STEP_COUNT] = {23, 19, 15, 9, 1};
}  // namespace

void HalFrontlight::begin(const uint8_t brightness, const uint8_t warmth, const bool on, const uint8_t dimStep) {
  if (!manager.present()) return;

  manager.begin();
  lastBrightness = brightness > 100 ? 100 : brightness;
  dimStepIdx = dimStep > DIM_STEP_COUNT ? DIM_STEP_COUNT : dimStep;
  manager.setColorTemperature(warmth > 100 ? 100 : warmth);
  lit = on;
  applyLit();
  LOG_INF("LIGHT", "Frontlight up: %u%% warm=%u%% dim=%u %s", lastBrightness, manager.colorTemperature(), dimStepIdx,
          lit ? "on" : "off");
}

void HalFrontlight::applyLit() {
  if (!lit) {
    manager.setBrightness(0);
  } else if (dimStepIdx > 0) {
    manager.setBrightnessLevel(DIM_LEVELS[dimStepIdx - 1]);
  } else {
    manager.setBrightness(lastBrightness);
  }
}

void HalFrontlight::setBrightness(const uint8_t percent) {
  lastBrightness = percent > 100 ? 100 : percent;
  dimStepIdx = 0;
  if (lit) applyLit();
}

void HalFrontlight::setDimStep(const uint8_t step) {
  dimStepIdx = step > DIM_STEP_COUNT ? DIM_STEP_COUNT : step;
  if (lit) applyLit();
}

void HalFrontlight::setWarmth(const uint8_t warmPercent) {
  manager.setColorTemperature(warmPercent > 100 ? 100 : warmPercent);
}

void HalFrontlight::setOn(const bool on) {
  if (on == lit) return;
  lit = on;
  applyLit();
}
