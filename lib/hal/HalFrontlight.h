#pragma once

#include <FrontlightManager.h>

// Thin firmware HAL over the SDK frontlight manager. It is inert on boards
// without a frontlight, so callers do not need board-specific conditionals.
class HalFrontlight {
 public:
  static HalFrontlight& getInstance() { return instance; }

  void begin(uint8_t brightness, uint8_t warmth, bool on, uint8_t dimStep = 0);

  bool present() const { return manager.present(); }
  bool hasColorTemperature() const { return manager.hasColorTemperature(); }

  void setBrightness(uint8_t percent);
  void setWarmth(uint8_t warmPercent);
  void setOn(bool on);

  // Night Light: sub-1% dim ladder below the panel's 1% floor, driven through
  // the SDK's perceptual setBrightnessLevel(). Step 1 is the brightest special
  // step, DIM_STEP_COUNT the dimmest (the hardware's minimum non-zero duty).
  // Entering a step leaves percent mode; setBrightness() returns to it.
  // LEDC frontlights only: the SDK's PM1 PMIC path (Paper Mono) does not honor
  // level mode — its percent gamma truncates the ladder and the dimmest step
  // would write duty 0 (light off). Gate every consumer on this.
  static constexpr uint8_t DIM_STEP_COUNT = 5;
  bool supportsNightLight() const { return manager.present() && !BoardConfig::ACTIVE.frontlight.viaPm1Pwm; }
  void setDimStep(uint8_t step);
  uint8_t dimStep() const { return dimStepIdx; }

  uint8_t brightness() const { return lastBrightness; }
  uint8_t warmth() const { return manager.colorTemperature(); }
  bool isOn() const { return lit; }

 private:
  HalFrontlight() = default;

  void applyLit();

  FrontlightManager manager;
  // The SDK represents off as brightness 0. Keep the selected brightness so
  // toggling back on restores it.
  uint8_t lastBrightness = 60;
  uint8_t dimStepIdx = 0;  // 0 = percent mode, 1..DIM_STEP_COUNT = night steps
  bool lit = false;

  static HalFrontlight instance;
};

#define Frontlight HalFrontlight::getInstance()
