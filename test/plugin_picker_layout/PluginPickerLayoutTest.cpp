#include <EpdFont.h>
#include <builtinFonts/ubuntu_10_regular.h>
#include <builtinFonts/ubuntu_12_regular.h>
#include <gtest/gtest.h>

#include <string>

#include "util/PluginPickerLayout.h"

namespace fui = freeink::ui;

namespace {
class FontTarget final : public fui::DrawTarget {
 public:
  fui::Size measureText(fui::FontId font, const char* text, fui::TextStyle) const override {
    int width = 0;
    int height = 0;
    (font == 0 ? small : body).getTextDimensions(text, &width, &height);
    return {static_cast<int16_t>(width), lineHeight(font)};
  }
  int16_t lineHeight(fui::FontId font) const override {
    return font == 0 ? ubuntu_10_regular.advanceY : ubuntu_12_regular.advanceY;
  }
  void fill(fui::Rect, fui::Paint, uint8_t, uint8_t) override {}
  void stroke(fui::Rect, fui::Paint, uint8_t, uint8_t, uint8_t) override {}
  void line(fui::Point, fui::Point, uint8_t, fui::Paint) override {}
  void triangle(fui::Point, fui::Point, fui::Point, fui::Paint) override {}
  void text(fui::Rect, const char*, fui::TextStyle) override {}
  void bitmap(fui::Rect, fui::BitmapRef, fui::BitmapMode, fui::Paint, fui::Rotation) override {}

 private:
  EpdFont small{&ubuntu_10_regular};
  EpdFont body{&ubuntu_12_regular};
};

fui::ListProps pickerProps() {
  fui::ListProps props;
  props.labelText.font = 1;
  props.labelText.maxLines = 2;
  props.subtitleText.font = 0;
  props.subtitleText.maxLines = 2;
  props.valueText.font = 0;
  props.rowPaddingY = 8;
  props.rowHeight = 48;
  props.sidePadding = 16;
  return props;
}

std::string wrapped(const FontTarget& target, const char* text, const fui::ListProps& props, int width) {
  std::string result;
  fui::layoutText(target, {0, 0, static_cast<int16_t>(width), 1}, text, props.subtitleText,
                  [&result](const char* line, fui::Rect) {
                    if (!result.empty()) result += ' ';
                    result += line;
                  });
  return result;
}
}  // namespace

TEST(PluginPickerLayout, AllFiveEventsRemainReadableWithFirmwareFonts) {
  const FontTarget target;
  static constexpr const char* DISCLOSURES[] = {
      "Receives: book opens, book closes and progress, reading sessions, downloads, sleep and current book",
      "Recibe: apertura de libros, cierre y progreso, sesiones de lectura, descargas, reposo y libro actual",
      ("Rep: obertura de llibres, tancament i progr\xc3\xa9s, sessions de lectura, baixades, rep\xc3\xb2s i llibre "
       "actual")};
  for (const char* disclosure : DISCLOSURES) {
    for (const int width : {240, 384, 480, 800}) {
      auto props = pickerProps();
      const int contentWidth = width - props.sidePadding * 2;
      if (width <= 384) EXPECT_NE(wrapped(target, disclosure, props, contentWidth), disclosure);
      fitPluginSubtitleToViewport(target, width < 480 ? 320 : 200, props);
      EXPECT_EQ(wrapped(target, disclosure, props, contentWidth), disclosure) << width;
    }
  }
}

TEST(PluginPickerLayout, LongDescriptionsAndWrappedTitlesFitWithinViewport) {
  const FontTarget target;
  std::string description;
  for (int i = 0; i < 100; ++i) description += "Long plugin description. ";
  const fui::ListItem item{"A plugin with a long title that wraps over two lines", description.c_str(), ">"};
  for (const int height : {160, 200, 320, 640}) {
    for (const int width : {240, 384, 480, 800}) {
      auto props = pickerProps();
      fitPluginSubtitleToViewport(target, height, props);
      const auto layout = fui::measureListRow(target, nullptr, width, props, item);
      EXPECT_LE(layout.height, height) << width << "x" << height;
      EXPECT_LE(props.subtitleText.maxLines, 16);
    }
  }
}

TEST(PluginPickerLayout, ShortSubtitlesKeepTheirOriginalRowHeight) {
  const FontTarget target;
  const fui::ListItem item{"Plugin", "One event", ">"};
  auto props = pickerProps();
  const auto before = fui::measureListRow(target, nullptr, 480, props, item);
  fitPluginSubtitleToViewport(target, 640, props);
  const auto after = fui::measureListRow(target, nullptr, 480, props, item);
  EXPECT_EQ(after.height, before.height);
}
