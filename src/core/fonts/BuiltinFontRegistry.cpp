#include "core/fonts/BuiltinFontRegistry.h"

#include <EpdFont.h>
#include <FeatureFlags.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <builtinFonts/all.h>

#include "fontIds.h"

namespace core {
namespace {

FontDecompressor fontDecompressor;
bool fontDecompressorInitAttempted = false;
bool fontDecompressorReady = false;

EpdFont bookerly14RegularFont(&bookerly_14_regular);
EpdFont bookerly14BoldFont(&bookerly_14_bold);
EpdFont bookerly14ItalicFont(&bookerly_14_italic);
EpdFont bookerly14BoldItalicFont(&bookerly_14_bolditalic);
EpdFontFamily bookerly14FontFamily(&bookerly14RegularFont, &bookerly14BoldFont, &bookerly14ItalicFont,
                                   &bookerly14BoldItalicFont);

#if ENABLE_BOOKERLY_FONTS
EpdFont bookerly12RegularFont(&bookerly_12_regular);
EpdFont bookerly12BoldFont(&bookerly_12_bold);
EpdFont bookerly12ItalicFont(&bookerly_12_italic);
EpdFont bookerly12BoldItalicFont(&bookerly_12_bolditalic);
EpdFontFamily bookerly12FontFamily(&bookerly12RegularFont, &bookerly12BoldFont, &bookerly12ItalicFont,
                                   &bookerly12BoldItalicFont);

EpdFont bookerly16RegularFont(&bookerly_16_regular);
EpdFont bookerly16BoldFont(&bookerly_16_bold);
EpdFont bookerly16ItalicFont(&bookerly_16_italic);
EpdFont bookerly16BoldItalicFont(&bookerly_16_bolditalic);
EpdFontFamily bookerly16FontFamily(&bookerly16RegularFont, &bookerly16BoldFont, &bookerly16ItalicFont,
                                   &bookerly16BoldItalicFont);

EpdFont bookerly18RegularFont(&bookerly_18_regular);
EpdFont bookerly18BoldFont(&bookerly_18_bold);
EpdFont bookerly18ItalicFont(&bookerly_18_italic);
EpdFont bookerly18BoldItalicFont(&bookerly_18_bolditalic);
EpdFontFamily bookerly18FontFamily(&bookerly18RegularFont, &bookerly18BoldFont, &bookerly18ItalicFont,
                                   &bookerly18BoldItalicFont);
#endif

#if ENABLE_NOTOSANS_FONTS
EpdFont notosans12RegularFont(&notosans_12_regular);
EpdFont notosans12BoldFont(&notosans_12_bold);
EpdFont notosans12ItalicFont(&notosans_12_italic);
EpdFont notosans12BoldItalicFont(&notosans_12_bolditalic);
EpdFontFamily notosans12FontFamily(&notosans12RegularFont, &notosans12BoldFont, &notosans12ItalicFont,
                                   &notosans12BoldItalicFont);

EpdFont notosans14RegularFont(&notosans_14_regular);
EpdFont notosans14BoldFont(&notosans_14_bold);
EpdFont notosans14ItalicFont(&notosans_14_italic);
EpdFont notosans14BoldItalicFont(&notosans_14_bolditalic);
EpdFontFamily notosans14FontFamily(&notosans14RegularFont, &notosans14BoldFont, &notosans14ItalicFont,
                                   &notosans14BoldItalicFont);

EpdFont notosans16RegularFont(&notosans_16_regular);
EpdFont notosans16BoldFont(&notosans_16_bold);
EpdFont notosans16ItalicFont(&notosans_16_italic);
EpdFont notosans16BoldItalicFont(&notosans_16_bolditalic);
EpdFontFamily notosans16FontFamily(&notosans16RegularFont, &notosans16BoldFont, &notosans16ItalicFont,
                                   &notosans16BoldItalicFont);

EpdFont notosans18RegularFont(&notosans_18_regular);
EpdFont notosans18BoldFont(&notosans_18_bold);
EpdFont notosans18ItalicFont(&notosans_18_italic);
EpdFont notosans18BoldItalicFont(&notosans_18_bolditalic);
EpdFontFamily notosans18FontFamily(&notosans18RegularFont, &notosans18BoldFont, &notosans18ItalicFont,
                                   &notosans18BoldItalicFont);
#endif

#if ENABLE_OPENDYSLEXIC_FONTS
EpdFont opendyslexic8RegularFont(&opendyslexic_8_regular);
EpdFont opendyslexic8BoldFont(&opendyslexic_8_bold);
EpdFont opendyslexic8ItalicFont(&opendyslexic_8_italic);
EpdFont opendyslexic8BoldItalicFont(&opendyslexic_8_bolditalic);
EpdFontFamily opendyslexic8FontFamily(&opendyslexic8RegularFont, &opendyslexic8BoldFont, &opendyslexic8ItalicFont,
                                      &opendyslexic8BoldItalicFont);

EpdFont opendyslexic10RegularFont(&opendyslexic_10_regular);
EpdFont opendyslexic10BoldFont(&opendyslexic_10_bold);
EpdFont opendyslexic10ItalicFont(&opendyslexic_10_italic);
EpdFont opendyslexic10BoldItalicFont(&opendyslexic_10_bolditalic);
EpdFontFamily opendyslexic10FontFamily(&opendyslexic10RegularFont, &opendyslexic10BoldFont, &opendyslexic10ItalicFont,
                                       &opendyslexic10BoldItalicFont);

EpdFont opendyslexic12RegularFont(&opendyslexic_12_regular);
EpdFont opendyslexic12BoldFont(&opendyslexic_12_bold);
EpdFont opendyslexic12ItalicFont(&opendyslexic_12_italic);
EpdFont opendyslexic12BoldItalicFont(&opendyslexic_12_bolditalic);
EpdFontFamily opendyslexic12FontFamily(&opendyslexic12RegularFont, &opendyslexic12BoldFont, &opendyslexic12ItalicFont,
                                       &opendyslexic12BoldItalicFont);

EpdFont opendyslexic14RegularFont(&opendyslexic_14_regular);
EpdFont opendyslexic14BoldFont(&opendyslexic_14_bold);
EpdFont opendyslexic14ItalicFont(&opendyslexic_14_italic);
EpdFont opendyslexic14BoldItalicFont(&opendyslexic_14_bolditalic);
EpdFontFamily opendyslexic14FontFamily(&opendyslexic14RegularFont, &opendyslexic14BoldFont, &opendyslexic14ItalicFont,
                                       &opendyslexic14BoldItalicFont);
#endif

EpdFont smallFont(&notosans_8_regular);
EpdFontFamily smallFontFamily(&smallFont);

EpdFont ui10RegularFont(&ubuntu_10_regular);
EpdFont ui10BoldFont(&ubuntu_10_bold);
EpdFontFamily ui10FontFamily(&ui10RegularFont, &ui10BoldFont);

EpdFont ui12RegularFont(&ubuntu_12_regular);
EpdFont ui12BoldFont(&ubuntu_12_bold);
EpdFontFamily ui12FontFamily(&ui12RegularFont, &ui12BoldFont);

void registerUiFontFamilies(GfxRenderer& renderer) {
  renderer.insertFontFamily(UI_10_FONT_ID, &ui10FontFamily);
  renderer.insertFontFamily(UI_12_FONT_ID, &ui12FontFamily);
  renderer.insertFontFamily(SMALL_FONT_ID, &smallFontFamily);
}

bool ensureFontDecompressorReady() {
  if (!fontDecompressorInitAttempted) {
    fontDecompressorInitAttempted = true;
    fontDecompressorReady = fontDecompressor.init();
    if (!fontDecompressorReady) {
      LOG_ERR("FONTS", "Font decompressor init failed");
    }
  }
  return fontDecompressorReady;
}

}  // namespace

void BuiltinFontRegistry::registerUiFonts(GfxRenderer& renderer) { registerUiFontFamilies(renderer); }

bool BuiltinFontRegistry::registerAllFonts(GfxRenderer& renderer) {
  registerUiFontFamilies(renderer);

  if (!ensureFontDecompressorReady()) {
    return false;
  }

  renderer.setFontDecompressor(&fontDecompressor);
  renderer.insertFontFamily(BOOKERLY_14_FONT_ID, &bookerly14FontFamily);

#if ENABLE_BOOKERLY_FONTS
  renderer.insertFontFamily(BOOKERLY_12_FONT_ID, &bookerly12FontFamily);
  renderer.insertFontFamily(BOOKERLY_16_FONT_ID, &bookerly16FontFamily);
  renderer.insertFontFamily(BOOKERLY_18_FONT_ID, &bookerly18FontFamily);
#endif

#if ENABLE_NOTOSANS_FONTS
  renderer.insertFontFamily(NOTOSANS_12_FONT_ID, &notosans12FontFamily);
  renderer.insertFontFamily(NOTOSANS_14_FONT_ID, &notosans14FontFamily);
  renderer.insertFontFamily(NOTOSANS_16_FONT_ID, &notosans16FontFamily);
  renderer.insertFontFamily(NOTOSANS_18_FONT_ID, &notosans18FontFamily);
#endif

#if ENABLE_OPENDYSLEXIC_FONTS
  renderer.insertFontFamily(OPENDYSLEXIC_8_FONT_ID, &opendyslexic8FontFamily);
  renderer.insertFontFamily(OPENDYSLEXIC_10_FONT_ID, &opendyslexic10FontFamily);
  renderer.insertFontFamily(OPENDYSLEXIC_12_FONT_ID, &opendyslexic12FontFamily);
  renderer.insertFontFamily(OPENDYSLEXIC_14_FONT_ID, &opendyslexic14FontFamily);
#endif

  return true;
}

}  // namespace core
