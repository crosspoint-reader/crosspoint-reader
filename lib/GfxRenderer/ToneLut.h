#pragma once

#include <cstdint>

// Optional tone adjustments for the BMP decode pipeline. Plain data + free
// functions; no globals, no Arduino dependency (host-testable).
//
// Formula and fixed application order:
//   1. brightness (multiplicative):  v = round(in * B / 100)
//   2. gamma (normal definition):    v = round(255 * powf(v / 255, G))
//   3. contrast (around 128):        v = round(128 + (v - 128) * C / 100)
//   clamped to 0..255 after every step.
// Identity at B=100, G=1.00, C=100 (exact pass-through; steps are skipped).
// Gamma direction: G > 1.00 DARKENS midtones (e.g. in=128, G=1.30 -> ~103);
// G < 1.00 brightens them (in=128, G=0.70 -> ~153).
//
// `quantizer` selects the 4-level quantizer thresholds for this Bitmap:
//   0 = legacy (30/55/150, error refs 15/35/90/210) — the X4C/UC8279
//       viewer default (originalThresholds=false);
//   1 = canonical (43/128/213, target levels 0/85/170/255) — the SSD1677
//       viewer default (originalThresholds=true);
//   -1 = no override: keep the Bitmap constructor's originalThresholds value.
//
// Consumers: Bitmap (decode path) via the ToneLut the caller passes in, and
// the persisted render profiles below. A nullptr tone (or enabled=false) is
// an exact pass-through of the baseline pipeline; EPUB rendering, JPEG
// covers, PNG, thumbnails and the panel waveforms have their own decode
// paths and are untouched.

struct ToneLut {
  bool enabled = false;
  uint8_t brightnessPct = 100;  // 70..110, step 5
  uint8_t gammaPct = 100;       // gamma * 100: 70..130, step 5 (100 = gamma 1.00)
  uint8_t contrastPct = 100;    // 80..130, step 5
  int8_t quantizer = -1;        // -1 = fallback, 0 = legacy, 1 = canonical
  uint8_t map[256] = {};        // valid iff enabled; recomputed by buildToneLut()
};

// (Re)compute map[] from the current B/G/C. Cheap: 256 entries, powf only here.
void buildToneLut(ToneLut& lut);

// Quantizer selection for Bitmap::parseHeaders(): the tone override when set,
// otherwise the constructor fallback (current behavior).
bool toneLutQuantizerCanonical(const ToneLut* tone, bool fallback);

// Image Settings value ranges (the constraints the tone formula consumes;
// the UI step policy lives in the Image Settings input layer, not here).
constexpr uint8_t TONE_BRIGHTNESS_MIN = 70;
constexpr uint8_t TONE_BRIGHTNESS_MAX = 110;
constexpr uint8_t TONE_GAMMA_MIN = 70;   // gamma 0.70 * 100
constexpr uint8_t TONE_GAMMA_MAX = 130;  // gamma 1.30 * 100
constexpr uint8_t TONE_CONTRAST_MIN = 80;
constexpr uint8_t TONE_CONTRAST_MAX = 130;

// Persisted render profile: pure data, shared by the settings store (viewer
// and sleep), the staged draft and the renderers. ToneLut above remains the
// derived rendering implementation; the profile carries no persistence
// concerns and no map.
struct ToneProfile {
  uint8_t brightnessPct = 100;  // 70..110, step 5
  uint8_t gammaPct = 100;       // gamma * 100: 70..130, step 5
  uint8_t contrastPct = 100;    // 80..130, step 5
  int8_t quantizer = -1;        // -1 = no override (UI labels it "Default"), 0 = legacy, 1 = canonical
  bool operator==(const ToneProfile&) const = default;
};

// Clamp a profile into the settings ranges; out-of-range quantizer folds to
// the default (-1). Shared by the settings save and load paths so the
// constraints live in exactly one place.
void normalizeToneProfile(ToneProfile& profile);

// Rendering ToneLut for a profile (caller runs buildToneLut() before decode).
ToneLut toneLutFromProfile(const ToneProfile& profile);

// Persisted subset of a ToneLut (the profile save path input).
ToneProfile toneProfileFromTone(const ToneLut& tone);
