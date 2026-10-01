#include "ToneLut.h"

#include <algorithm>
#include <cmath>

// See ToneLut.h for the formula and the fixed B -> G -> C order.
void buildToneLut(ToneLut& lut) {
  const bool brightnessActive = lut.brightnessPct != 100;
  const bool gammaActive = lut.gammaPct != 100;
  const bool contrastActive = lut.contrastPct != 100;
  lut.enabled = brightnessActive || gammaActive || contrastActive;
  if (!lut.enabled) {
    for (int i = 0; i < 256; i++) lut.map[i] = static_cast<uint8_t>(i);
    return;
  }

  const float brightness = static_cast<float>(lut.brightnessPct) / 100.0f;
  const float gamma = static_cast<float>(lut.gammaPct) / 100.0f;
  const float contrast = static_cast<float>(lut.contrastPct) / 100.0f;

  for (int i = 0; i < 256; i++) {
    float v = static_cast<float>(i);
    // 1. Brightness: multiplicative.
    if (brightnessActive) {
      v *= brightness;
      if (v > 255.0f) v = 255.0f;
      if (v < 0.0f) v = 0.0f;
    }
    // 2. Gamma: out = 255 * (v/255)^G; G > 1 darkens midtones, G < 1 brightens.
    if (gammaActive) {
      v = 255.0f * powf(v / 255.0f, gamma);
      if (v > 255.0f) v = 255.0f;
      if (v < 0.0f) v = 0.0f;
    }
    // 3. Contrast: around the midpoint 128.
    if (contrastActive) {
      v = 128.0f + (v - 128.0f) * contrast;
      if (v > 255.0f) v = 255.0f;
      if (v < 0.0f) v = 0.0f;
    }
    lut.map[i] = static_cast<uint8_t>(v + 0.5f);
  }
}

bool toneLutQuantizerCanonical(const ToneLut* tone, bool fallback) {
  if (tone == nullptr || tone->quantizer < 0) return fallback;
  return tone->quantizer != 0;
}

void normalizeToneProfile(ToneProfile& profile) {
  // Plain range clamp (no UI step here — the step policy lives in the Image
  // Settings input layer).
  profile.brightnessPct = std::clamp<uint8_t>(profile.brightnessPct, TONE_BRIGHTNESS_MIN, TONE_BRIGHTNESS_MAX);
  profile.gammaPct = std::clamp<uint8_t>(profile.gammaPct, TONE_GAMMA_MIN, TONE_GAMMA_MAX);
  profile.contrastPct = std::clamp<uint8_t>(profile.contrastPct, TONE_CONTRAST_MIN, TONE_CONTRAST_MAX);
  // Only the explicit overrides are meaningful persisted; anything else folds
  // to the default (-1), matching the settings-load convention of folding
  // out-of-range values to the field default.
  if (profile.quantizer != 0 && profile.quantizer != 1) profile.quantizer = -1;
}

ToneLut toneLutFromProfile(const ToneProfile& profile) {
  ToneLut lut;
  lut.brightnessPct = profile.brightnessPct;
  lut.gammaPct = profile.gammaPct;
  lut.contrastPct = profile.contrastPct;
  lut.quantizer = profile.quantizer;
  return lut;  // enabled + map come from buildToneLut()
}

ToneProfile toneProfileFromTone(const ToneLut& tone) {
  ToneProfile profile;
  profile.brightnessPct = tone.brightnessPct;
  profile.gammaPct = tone.gammaPct;
  profile.contrastPct = tone.contrastPct;
  profile.quantizer = (tone.quantizer == 0 || tone.quantizer == 1) ? tone.quantizer : -1;
  return profile;
}
