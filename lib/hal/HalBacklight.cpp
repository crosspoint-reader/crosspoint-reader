#include "HalBacklight.h"

HalBacklight halBacklight;

void HalBacklight::begin() {
  if (!available()) return;
  pinMode(BoardConfig::ACTIVE.frontlight.pin, OUTPUT);
  setBrightness(0);
}

bool HalBacklight::available() const {
#if defined(HAS_PWM_BACKLIGHT)
  return BoardConfig::hasPwmFrontlight();
#else
  return false;
#endif
}

void HalBacklight::setBrightness(uint8_t percent) {
  if (!available()) return;
  if (percent > 100) percent = 100;
  brightnessPercent = percent;

  const auto& cfg = BoardConfig::ACTIVE.frontlight;
  const uint32_t maxDuty = (1UL << cfg.pwmResolutionBits) - 1UL;
  uint32_t duty = (maxDuty * percent) / 100U;
  if (!cfg.activeHigh) duty = maxDuty - duty;

  analogWriteFrequency(cfg.pin, cfg.pwmFrequencyHz);
  analogWriteResolution(cfg.pin, cfg.pwmResolutionBits);
  analogWrite(cfg.pin, duty);
}
