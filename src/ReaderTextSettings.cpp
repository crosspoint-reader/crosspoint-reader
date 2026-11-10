#include "ReaderTextSettings.h"

#include <Logging.h>

#include <cstring>
#include <tuple>

#include "CrossPointSettings.h"
#include "ReaderLineSpacing.h"

namespace {
void apply(const ReaderTextSettings& text) {
  auto& settings = SETTINGS;
  std::memcpy(settings.sdFontFamilyName, text.sdFontFamilyName, sizeof(text.sdFontFamilyName));
  settings.fontFamily = CrossPointSettings::NOTOSANS;
  settings.fontPointSize = text.fontPointSize;
  settings.lineSpacing = text.lineSpacing;
  settings.wordSpacing = text.wordSpacing;
  settings.characterSpacing = text.characterSpacing;
  settings.extraParagraphSpacing = text.extraParagraphSpacing;
  settings.paragraphIndentSpaces = text.paragraphIndentSpaces;
  settings.paragraphAlignment = text.paragraphAlignment;
  settings.screenMargin = text.screenMargin;
  settings.focusReadingEnabled = text.focusReadingEnabled;
  settings.hyphenationEnabled = text.hyphenationEnabled;
  settings.embeddedStyle = text.embeddedStyle;
  settings.textAntiAliasing = text.textAntiAliasing;
}
}  // namespace

ReaderTextSettings ReaderTextSettings::defaults() {
  const auto& settings = SETTINGS;
  ReaderTextSettings text;
  std::memcpy(text.sdFontFamilyName, settings.sdFontFamilyName, sizeof(text.sdFontFamilyName));
  text.sdFontFamilyName[sizeof(text.sdFontFamilyName) - 1] = '\0';
  text.fontPointSize = settings.fontPointSize;
  text.lineSpacing = settings.lineSpacing;
  text.wordSpacing = settings.wordSpacing;
  text.characterSpacing = settings.characterSpacing;
  text.extraParagraphSpacing = settings.extraParagraphSpacing;
  text.paragraphIndentSpaces = settings.paragraphIndentSpaces;
  text.paragraphAlignment = settings.paragraphAlignment;
  text.screenMargin = settings.screenMargin;
  text.focusReadingEnabled = settings.focusReadingEnabled;
  text.hyphenationEnabled = settings.hyphenationEnabled;
  text.embeddedStyle = settings.embeddedStyle;
  text.textAntiAliasing = settings.textAntiAliasing;
  return text;
}

bool ReaderTextSettings::saveDefaults() const {
  if (!isValid()) {
    LOG_ERR("TXT", "Invalid default text settings");
    return false;
  }
  const auto previous = defaults();
  if (*this == previous) return true;
  apply(*this);
  if (SETTINGS.saveToFile()) return true;
  apply(previous);
  LOG_ERR("TXT", "Could not save default text settings");
  return false;
}

bool ReaderTextSettings::isValid() const {
  if (!std::memchr(sdFontFamilyName, '\0', sizeof(sdFontFamilyName))) return false;
  for (const char* p = sdFontFamilyName; *p; ++p) {
    if (static_cast<unsigned char>(*p) < 32 || *p == '/' || *p == '\\') return false;
  }
  return fontPointSize > CrossPointSettings::LEGACY_FONT_SIZE_MAX &&
         lineSpacing < CrossPointSettings::LINE_COMPRESSION_COUNT &&
         wordSpacing >= CrossPointSettings::WORD_SPACING_MIN && wordSpacing <= CrossPointSettings::WORD_SPACING_MAX &&
         characterSpacing <= 4 && extraParagraphSpacing <= 1 && paragraphIndentSpaces <= 5 &&
         paragraphAlignment < CrossPointSettings::PARAGRAPH_ALIGNMENT_COUNT &&
         screenMargin >= CrossPointSettings::SCREEN_MARGIN_MIN &&
         screenMargin <= CrossPointSettings::SCREEN_MARGIN_MAX && focusReadingEnabled <= 1 && hyphenationEnabled <= 1 &&
         embeddedStyle <= 1 && textAntiAliasing <= 1;
}

int8_t ReaderTextSettings::getCharacterSpacing() const {
  return static_cast<int8_t>(characterSpacing - CrossPointSettings::CHARACTER_SPACING_OFFSET);
}

float ReaderTextSettings::getReaderLineCompression() const {
  return LINE_SPACING_MULTIPLIERS[lineSpacing < CrossPointSettings::LINE_COMPRESSION_COUNT
                                      ? lineSpacing
                                      : static_cast<uint8_t>(CrossPointSettings::LINE_SPACING_120)];
}

ReaderRenderSpec ReaderTextSettings::renderSpec(const int fontId, const uint16_t width, const uint16_t height,
                                                const uint8_t imageRendering) const {
  ReaderRenderSpec spec;
  spec.fontId = fontId;
  spec.lineCompression = getReaderLineCompression();
  spec.characterSpacing = getCharacterSpacing();
  spec.wordSpacingPercent = wordSpacing;
  spec.extraParagraphSpacing = extraParagraphSpacing != 0;
  spec.paragraphIndentSpaces = paragraphIndentSpaces;
  spec.paragraphAlignment = paragraphAlignment;
  spec.viewportWidth = width;
  spec.viewportHeight = height;
  spec.hyphenationEnabled = hyphenationEnabled != 0;
  spec.embeddedStyle = embeddedStyle != 0;
  spec.imageRendering = imageRendering == CrossPointSettings::IMAGES_DISPLAY_BW
                            ? static_cast<uint8_t>(CrossPointSettings::IMAGES_DISPLAY)
                            : imageRendering;
  spec.focusReadingEnabled = focusReadingEnabled != 0;
  return spec;
}

bool ReaderTextSettings::operator==(const ReaderTextSettings& other) const {
  return std::strncmp(sdFontFamilyName, other.sdFontFamilyName, sizeof(sdFontFamilyName)) == 0 &&
         std::tie(fontPointSize, lineSpacing, wordSpacing, characterSpacing, extraParagraphSpacing,
                  paragraphIndentSpaces, paragraphAlignment, screenMargin, focusReadingEnabled, hyphenationEnabled,
                  embeddedStyle, textAntiAliasing) ==
             std::tie(other.fontPointSize, other.lineSpacing, other.wordSpacing, other.characterSpacing,
                      other.extraParagraphSpacing, other.paragraphIndentSpaces, other.paragraphAlignment,
                      other.screenMargin, other.focusReadingEnabled, other.hyphenationEnabled, other.embeddedStyle,
                      other.textAntiAliasing);
}
