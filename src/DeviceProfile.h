#pragma once

#include <cstdint>

#include "components/themes/BaseTheme.h"

struct DeviceProfile {
  const char* name;
  bool touchPrimary;
  bool showButtonHints;
  bool denseLightGrayDither;
  // Two-pass grayscale anti-aliasing requires hardware that can hold 4
  // distinct intensity levels via the LSB/MSB plane scheme. Murphy's
  // UC8253 panel has asymmetric drive rails and only produces clean
  // results in B/W mode — skip the AA pass and render in 1-bit instead.
  bool supportsGrayscaleAntiAlias;

  const ThemeMetrics* lyraMetrics;

  int coverThumbScale;
  int emptyCoverIconSize;
  int recentTitleFontId;
  int recentAuthorFontId;
  int emptyRecentsTitleFontId;
  int emptyRecentsSubtitleFontId;

  int mainMenuIconDrawSize;
  int mainMenuTextInset;
  int mainMenuLabelFontId;
  bool rotateMainMenuIconsClockwise;

  // Reinterprets the existing user font-size setting for panels with different
  // effective density. The setting still persists normally; this only shifts
  // the font asset chosen at render/cache time.
  int readerFontSizeDelta;
};

namespace DeviceProfiles {
const DeviceProfile& current();
uint8_t effectiveReaderFontSize();
int readerFontId();
int readerScreenMargin();
}  // namespace DeviceProfiles
