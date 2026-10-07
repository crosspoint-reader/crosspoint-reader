#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

#include "lib/GfxRenderer/BitmapHelpers.h"
#include "lib/GfxRenderer/ToneLut.h"

namespace {

ToneLut makeLut(int brightnessPct, int gammaPct, int contrastPct) {
  ToneLut lut{};
  lut.brightnessPct = static_cast<uint8_t>(brightnessPct);
  lut.gammaPct = static_cast<uint8_t>(gammaPct);
  lut.contrastPct = static_cast<uint8_t>(contrastPct);
  buildToneLut(lut);
  return lut;
}

}  // namespace

// The tone LUT is the single B/G/C implementation for both the image viewer
// and the sleep renderer; identity values must be an exact pass-through so
// old installations (no profiles, defaults) keep rendering unchanged.
TEST(ToneLut, IdentityLutIsExactPassThrough) {
  const ToneLut lut = makeLut(100, 100, 100);
  EXPECT_FALSE(lut.enabled);  // consumer keeps the baseline decode path
  for (int i = 0; i < 256; ++i) {
    EXPECT_EQ(lut.map[i], i) << "i=" << i;
  }
}

TEST(ToneLut, ClampedAtBothEnds) {
  const ToneLut max = makeLut(110, 130, 130);
  for (int i = 0; i < 256; ++i) {
    ASSERT_LE(max.map[i], 255) << "i=" << i;
    ASSERT_GE(max.map[i], 0) << "i=" << i;
  }
  EXPECT_EQ(max.map[255], 255);  // brightness 1.10 clips 255*1.10 -> 255; contrast clips 128+127*1.3 -> 255

  // With contrast disabled the poles stay pinned through brightness/gamma.
  const ToneLut pinned = makeLut(110, 130, 100);
  EXPECT_EQ(pinned.map[0], 0);      // 0 stays 0
  EXPECT_EQ(pinned.map[255], 255);  // clamped after brightness multiplication

  const ToneLut min = makeLut(70, 70, 80);
  for (int i = 0; i < 256; ++i) {
    ASSERT_LE(min.map[i], 255) << "i=" << i;
    ASSERT_GE(min.map[i], 0) << "i=" << i;
  }
}

TEST(ToneLut, BrightnessIsMultiplicative) {
  const ToneLut up = makeLut(110, 100, 100);
  EXPECT_EQ(up.map[100], 110);  // 100 * 1.10
  EXPECT_EQ(up.map[200], 220);  // 200 * 1.10
  EXPECT_EQ(up.map[240], 255);  // 264 clamps to 255
  const ToneLut down = makeLut(90, 100, 100);
  EXPECT_EQ(down.map[100], 90);  // 100 * 0.90
}

TEST(ToneLut, GammaAboveOneDarkensMidtonesBelowOneBrightens) {
  const ToneLut darker = makeLut(100, 130, 100);
  EXPECT_LT(darker.map[128], 128);  // 255*(128/255)^1.3 ~ 103
  EXPECT_LT(darker.map[60], 60);
  const ToneLut brighter = makeLut(100, 70, 100);
  EXPECT_GT(brighter.map[128], 128);  // 255*(128/255)^0.7 ~ 153
  EXPECT_GT(brighter.map[60], 60);
}

TEST(ToneLut, ContrastMovesAroundFixedMidpoint) {
  const ToneLut more = makeLut(100, 100, 130);
  EXPECT_EQ(more.map[128], 128);  // pivot unchanged
  EXPECT_EQ(more.map[64], 45);    // 128 + (64-128)*1.3 = 44.8
  EXPECT_EQ(more.map[192], 211);  // 128 + 64*1.3 = 211.2
  const ToneLut less = makeLut(100, 100, 80);
  EXPECT_EQ(less.map[128], 128);
  EXPECT_EQ(less.map[64], 77);  // 128 - 64*0.8 = 76.8
}

TEST(ToneLut, QuantizerOverrideSelection) {
  ToneLut tone{};
  // -1 = no override: falls back to the Bitmap constructor value
  EXPECT_EQ(toneLutQuantizerCanonical(&tone, false), false);
  EXPECT_EQ(toneLutQuantizerCanonical(&tone, true), true);
  EXPECT_EQ(toneLutQuantizerCanonical(nullptr, false), false);
  tone.quantizer = 0;  // legacy
  EXPECT_EQ(toneLutQuantizerCanonical(&tone, true), false);
  tone.quantizer = 1;  // canonical
  EXPECT_EQ(toneLutQuantizerCanonical(&tone, false), true);
}

TEST(ToneLut, QuantizerThresholdSetsDiverge) {
  // Legacy: 30/55/150. Canonical: 43/128/213.
  // gray=60: legacy -> level 2, canonical -> level 1.
  // gray=200: legacy -> level 3, canonical -> level 2.
  AtkinsonDitherer legacy(8, false);
  AtkinsonDitherer canonical(8, true);
  ASSERT_TRUE(legacy.isValid());
  ASSERT_TRUE(canonical.isValid());
  EXPECT_EQ(legacy.processPixel(60, 0), 2);
  EXPECT_EQ(canonical.processPixel(60, 0), 1);
  EXPECT_EQ(legacy.processPixel(200, 1), 3);
  EXPECT_EQ(canonical.processPixel(200, 1), 2);
}

// ---- ToneProfile: the persisted render profile type (viewer + sleep) ----
//
// Both profiles are ToneProfile values stored by CrossPointSettings
// (`viewerRenderProfile` / `sleepRenderProfile` objects). Their fromJson loads
// missing/absent fields as the struct defaults and clamp through
// normalizeToneProfile, so the tests below cover the load path's guarantees at
// the shared type level (CrossPointSettings itself needs device-side JSON and
// I18n/BoardConfig and is not host-buildable).

TEST(ToneProfile, DefaultsKeepRenderingUnchanged) {
  const ToneProfile profile{};
  EXPECT_EQ(profile, (ToneProfile{100, 100, 100, -1}));

  // The default profile must produce the SAME rendering decision a renderer
  // produces without any profile: identity LUT + fallback quantizer.
  ToneLut rendered = toneLutFromProfile(profile);
  buildToneLut(rendered);
  EXPECT_FALSE(rendered.enabled);                                 // baseline decode path (adjustPixel pass-through)
  EXPECT_EQ(toneLutQuantizerCanonical(&rendered, true), true);    // SSD1677 fallback preserved
  EXPECT_EQ(toneLutQuantizerCanonical(&rendered, false), false);  // legacy-panel fallback preserved
  for (int i = 0; i < 256; ++i) EXPECT_EQ(rendered.map[i], i) << "i=" << i;
}

TEST(ToneProfile, NormalizeClampsToSettingsRanges) {
  ToneProfile wild{255, 0, 200, 7};
  normalizeToneProfile(wild);
  EXPECT_EQ(wild.brightnessPct, TONE_BRIGHTNESS_MAX);
  EXPECT_EQ(wild.gammaPct, TONE_GAMMA_MIN);
  EXPECT_EQ(wild.contrastPct, TONE_CONTRAST_MAX);
  EXPECT_EQ(wild.quantizer, -1);  // out-of-range folds to default (settings-load convention)

  ToneProfile floor{0, 5, 10, 0};
  normalizeToneProfile(floor);
  EXPECT_EQ(floor.brightnessPct, TONE_BRIGHTNESS_MIN);
  EXPECT_EQ(floor.gammaPct, TONE_GAMMA_MIN);
  EXPECT_EQ(floor.contrastPct, TONE_CONTRAST_MIN);
  EXPECT_EQ(floor.quantizer, 0);

  ToneProfile valid{85, 90, 110, 1};
  normalizeToneProfile(valid);
  EXPECT_EQ(valid, (ToneProfile{85, 90, 110, 1}));  // in-range values pass through untouched
}

TEST(ToneProfile, NormalizeIsIdempotent) {
  ToneProfile a{200, 65, 140, 9};
  normalizeToneProfile(a);
  ToneProfile b = a;
  normalizeToneProfile(b);
  EXPECT_EQ(a, b);  // save -> load -> save keeps the same values
}

TEST(ToneProfile, ProfileRenderMatchesDirectLut) {
  // Same shared implementation: a ToneLut built from a persisted profile must
  // be identical to a ToneLut built directly from the same values.
  const ToneLut direct = makeLut(85, 90, 110);
  ToneLut fromProfile = toneLutFromProfile(ToneProfile{85, 90, 110, 1});
  buildToneLut(fromProfile);
  EXPECT_EQ(fromProfile.enabled, direct.enabled);
  EXPECT_EQ(0, memcmp(fromProfile.map, direct.map, sizeof(direct.map)));
  EXPECT_EQ(toneLutQuantizerCanonical(&fromProfile, true), toneLutQuantizerCanonical(&direct, true));

  const ToneLut directLegacy = makeLut(110, 70, 80);
  ToneLut legacyProfile = toneLutFromProfile(ToneProfile{110, 70, 80, 0});
  buildToneLut(legacyProfile);
  EXPECT_EQ(0, memcmp(legacyProfile.map, directLegacy.map, sizeof(directLegacy.map)));
  EXPECT_EQ(toneLutQuantizerCanonical(&legacyProfile, false), toneLutQuantizerCanonical(&directLegacy, false));
}

TEST(ToneProfile, ToneExtractionReadsExactlyItsInput) {
  // toneProfileFromTone converts an ACTIVE ToneLut back to profile form (the
  // settings-page staging path): explicit quantizer overrides are kept,
  // anything else folds to the "no override" default.
  ToneLut active = makeLut(85, 90, 110);
  active.quantizer = 1;
  EXPECT_EQ(toneProfileFromTone(active), (ToneProfile{85, 90, 110, 1}));

  active.quantizer = -1;  // untouched quantizer: stored as "no override"
  EXPECT_EQ(toneProfileFromTone(active), (ToneProfile{85, 90, 110, -1}));

  ToneLut garbage = makeLut(100, 100, 100);
  garbage.quantizer = 42;  // cannot happen via the UI; folds to the default
  EXPECT_EQ(toneProfileFromTone(garbage), (ToneProfile{100, 100, 100, -1}));
}

TEST(ToneProfile, SessionStartReproducesTheAppliedDraft) {
  // Session start: activeTone = toneLutFromProfile(persisted profile) +
  // buildToneLut. A saved non-default profile must reproduce the exact LUT
  // the staged draft had when it was applied.
  ToneLut draft = makeLut(85, 90, 110);
  draft.quantizer = 1;
  ToneProfile saved = toneProfileFromTone(draft);
  normalizeToneProfile(saved);
  EXPECT_EQ(saved, (ToneProfile{85, 90, 110, 1}));

  ToneLut sessionStart = toneLutFromProfile(saved);
  buildToneLut(sessionStart);
  EXPECT_EQ(sessionStart.enabled, draft.enabled);
  EXPECT_EQ(0, memcmp(sessionStart.map, draft.map, sizeof(draft.map)));
  EXPECT_EQ(toneLutQuantizerCanonical(&sessionStart, false), true);

  // Round trip through normalize is idempotent for UI-reachable values.
  ToneProfile again = toneProfileFromTone(sessionStart);
  normalizeToneProfile(again);
  EXPECT_EQ(again, saved);
}

TEST(ToneProfile, ResetStagesTheDefaultProfileWithNoOverrideQuantizer) {
  // "Reset to defaults" stages ToneProfile{}: the quantizer stays the internal
  // -1 (no override / controller default) — NOT an explicit legacy 0. The save
  // path preserves -1, so the persisted default keeps the fallback semantics
  // after a Reset+Apply.
  ToneProfile draft{};  // what resetDraft() stages
  EXPECT_EQ(draft, (ToneProfile{100, 100, 100, -1}));
  normalizeToneProfile(draft);
  EXPECT_EQ(draft, (ToneProfile{100, 100, 100, -1}));  // already normalized: save keeps -1
}
