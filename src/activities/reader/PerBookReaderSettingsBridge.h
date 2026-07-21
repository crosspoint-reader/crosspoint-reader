#pragma once

#include <cstring>

#include "CrossPointSettings.h"
#include "PerBookReaderSettings.h"

inline PerBookReaderSettings captureReaderSettings(const bool hasOverrides = false,
                                                   const bool hasAutoPageTurnInterval = false,
                                                   const uint8_t autoPageTurnSeconds = 0,
                                                   const bool autoPageTurnStartsOnOpen = false) {
  PerBookReaderSettings out;
  out.hasReaderOverrides = hasOverrides;
  out.hasAutoPageTurnInterval = hasAutoPageTurnInterval;
  out.autoPageTurnStartsOnOpen = hasAutoPageTurnInterval && autoPageTurnStartsOnOpen;
  out.fontFamily = SETTINGS.fontFamily;
  out.fontSize = SETTINGS.fontSize;
  out.lineSpacing = SETTINGS.lineSpacing;
  out.paragraphAlignment = SETTINGS.paragraphAlignment;
  out.orientation = SETTINGS.orientation;
  out.screenMargin = SETTINGS.screenMargin;
  out.embeddedStyle = SETTINGS.embeddedStyle;
  out.focusReadingEnabled = SETTINGS.focusReadingEnabled;
  out.hyphenationEnabled = SETTINGS.hyphenationEnabled;
  out.extraParagraphSpacing = SETTINGS.extraParagraphSpacing;
  out.textAntiAliasing = SETTINGS.textAntiAliasing;
  out.imageRendering = SETTINGS.imageRendering;
  out.autoPageTurnSeconds = hasAutoPageTurnInterval ? autoPageTurnSeconds : 0;
  std::strncpy(out.sdFontFamilyName.data(), SETTINGS.sdFontFamilyName, out.sdFontFamilyName.size() - 1);
  out.sdFontFamilyName.back() = '\0';
  return out;
}

inline void applyReaderSettings(const PerBookReaderSettings& settings) {
  SETTINGS.fontFamily = settings.fontFamily;
  SETTINGS.fontSize = settings.fontSize;
  SETTINGS.lineSpacing = settings.lineSpacing;
  SETTINGS.paragraphAlignment = settings.paragraphAlignment;
  SETTINGS.orientation = settings.orientation;
  SETTINGS.screenMargin = settings.screenMargin;
  SETTINGS.embeddedStyle = settings.embeddedStyle;
  SETTINGS.focusReadingEnabled = settings.focusReadingEnabled;
  SETTINGS.hyphenationEnabled = settings.hyphenationEnabled;
  SETTINGS.extraParagraphSpacing = settings.extraParagraphSpacing;
  SETTINGS.textAntiAliasing = settings.textAntiAliasing;
  SETTINGS.imageRendering = settings.imageRendering;
  std::strncpy(SETTINGS.sdFontFamilyName, settings.sdFontFamilyName.data(), sizeof(SETTINGS.sdFontFamilyName) - 1);
  SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
}
