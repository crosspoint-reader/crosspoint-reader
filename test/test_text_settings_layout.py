"""Run with python3 test/test_text_settings_layout.py (requires a host C++ compiler)."""

from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
activity = (ROOT / "src/activities/settings/TextSettingsActivity.cpp").read_text()
preview = (ROOT / "src/activities/settings/TextSettingsPreview.cpp").read_text()
english = (ROOT / "lib/I18n/translations/english.yaml").read_text()
tabs = (ROOT / "src/activities/UiTabListActivity.cpp").read_text()
lists = (ROOT / "src/activities/UiListActivity.cpp").read_text()
renderer = (ROOT / "lib/GfxRenderer/GfxRenderer.cpp").read_text()
theme = (ROOT / "src/components/UITheme.cpp").read_text()
tokens = (ROOT / "src/components/UIThemeTokens.h").read_text()


def method(source, signature):
    return signature + source.split(signature, 1)[1].split("\n}", 1)[0] + "\n}\n"


# Exercise production geometry with the real SDK screen and tab components,
# without SD storage or the activity manager. Text measurements use the real UI fonts.
style_labels = activity.split("constexpr StrId STYLE_ROW_NAME_IDS[] = {", 1)[1].split("};", 1)[0]
style_labels = re.sub(
    r"StrId::(STR_[A-Z_]+)",
    lambda match: english.split(match[1] + ": ", 1)[1].splitlines()[0],
    style_labels,
)
enter = activity.split("  metrics_ =", 1)[1].split("\n  fonts_.clear();", 1)[0]
build = activity.split("void TextSettingsActivity::buildScreen(UiScreen& screen) {", 1)[1]
rows = "  fui::ListProps props;" + build.split("  fui::ListProps props;", 1)[1]
rows = rows.split("\nconst char* TextSettingsActivity::confirmLabelText", 1)[0]
build = build.split("\n  // rowItems_", 1)[0]
sync = lists.split("void UiListActivity::syncListViewport(", 1)[1]
sync = "void TextSettingsActivity::syncListViewport(" + sync.split(
    "\nvoid UiListActivity::drawChrome", 1
)[0]
tab_bar = tabs.split("void UiTabListActivity::buildTabBar(UiScreen& screen) {", 1)[1]
chrome = activity.split("void TextSettingsActivity::drawChrome() {", 1)[1]
chrome = chrome.split("\nvoid TextSettingsActivity::drawFooter()", 1)[0]
preview_caption = "void renderPreview(" + preview.split("void renderPreview(", 1)[1]
preview_caption = preview_caption.split("\n  const int fontId =", 1)[0] + "\n}\n"
preview_caption = preview_caption.replace(
    "{\n",
    "{\n  renderer.preview = bounds;\n"
    "  assert((renderer.clip == std::array<int, 4>{bounds.x, bounds.y, bounds.width, bounds.height}));\n",
    1,
)
orientation = (ROOT / "lib/GfxRenderer/GfxRenderer.h").read_text()
orientation = "enum Orientation {" + orientation.split("enum Orientation {", 1)[1].split("\n  };", 1)[0] + "\n  };"

harness = r"""
#include <FreeInkApp.h>
#include <BoardConfig.h>
#include <EpdFont.h>
#include <builtinFonts/ubuntu_10_regular.h>
#include <builtinFonts/ubuntu_12_regular.h>
#include <components/themes/lyra/LyraTheme.h>
#include <components/themes/roundedraff/RoundedRaffTheme.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
namespace fui = freeink::ui;
using UiScreen = fui::Screen<32>;
struct GfxRenderer {
""" + orientation + r"""
  Orientation orientation = Portrait;
  int panelWidth = 800, panelHeight = 480;
  Rect header{};
  mutable Rect preview{}, caption{};
  mutable std::string captionText;
  mutable std::array<int, 4> clip{0, 0, 2000, 2000};
  int getScreenWidth() const;
  int getScreenHeight() const;
  Orientation getOrientation() const { return orientation; }
  void getOrientedViewableTRBL(int*, int*, int*, int*) const;
  int getTextHeight(int) const { return ubuntu_10_regular.ascender; }
  void drawText(int, int x, int y, const char* text) const {
    EpdFont font(&ubuntu_10_regular);
    int width, height;
    font.getTextDimensions(text, &width, &height);
    caption = Rect{x, y, width, ubuntu_10_regular.advanceY};
    captionText = text;
  }
  std::array<int, 4> getClipRect() const { return clip; }
  void setClipRect(int x, int y, int w, int h) const { clip = {x, y, w, h}; }
};
struct UITheme {
  ThemeMetrics metrics;
  static UITheme& getInstance() { static UITheme instance; return instance; }
  const ThemeMetrics& getMetrics() const { return metrics; }
  Rect getContentArea(const GfxRenderer&);
  Rect getScreenSafeArea(const GfxRenderer&, bool, bool);
  Rect getHeaderRect(const GfxRenderer&, bool, bool);
  void drawHeader(GfxRenderer& renderer, Rect rect, const char*) { renderer.header = rect; }
};
#define GUI UITheme::getInstance()
#define tr(id) id
constexpr const char* STR_TEXT_SETTINGS = "Text Settings";
constexpr const char* STR_PREVIEW = """ + english.split("STR_PREVIEW: ", 1)[1].splitlines()[0] + r""";
constexpr int UI_10_FONT_ID = 0, TOUCH_TAB_BAR_HEIGHT = 50, ACTION_TAB = 1, ACTION_ROW = 2;
namespace textsettings {
  struct PreviewLayout {};
""" + preview_caption + r"""
}
struct UiTabListActivity {
  struct { bool touch = false; bool hasTouch() const { return touch; } } mappedInput;
  int tabPillMaxPad = 0, selected = 0;
  int tabCount() const { return 4; }
  const char* tabLabel(int index) const {
    static const char* labels[] = {"Font", "Size", "Layout", "Style"};
    return labels[index];
  }
  virtual int activeTab() const = 0;
  fui::TabIndicator tabIndicator(int) const { return fui::TabIndicator::None; }
  int ringPos() const { return selected; }
  void buildTabBar(UiScreen&);
};
struct TextSettingsActivity : UiTabListActivity {
  enum class Tab { Family, Size, Layout, Style };
  Tab tab_ = Tab::Layout;
  int activeTab() const override { return static_cast<int>(tab_); }
  GfxRenderer renderer;
  ThemeMetrics metrics_;
  int afterHeader = 0, previewHeight = 0;
  struct Entry { std::string name; };
  std::vector<Entry> fonts_{{"Bitter"}}, sizes_{{"12 pt"}};
  int currentFamilyIndex_ = 0, currentSizeIndex_ = 0;
  textsettings::PreviewLayout previewLayout_;
  std::vector<fui::ListItem> rowItems_;
  fui::ListNav nav;
  fui::ListNav& activeNav() { return nav; }
  int listCount() const { return rowItems_.size(); }
  void syncListViewport(UiScreen&, fui::ListProps&, int);
  void syncTabListViewport(UiScreen& screen, fui::ListProps& props) {
    syncListViewport(screen, props, 1);
  }
  void enterGeometry() {
    metrics_ =""" + enter + r"""
  }
  void buildScreen(UiScreen& screen) {""" + build + r"""
  }
  void buildRows(UiScreen& screen) {""" + rows + r"""
  void drawChrome() {""" + chrome + r"""
};
""" + sync + method(
    renderer, "int GfxRenderer::getScreenWidth() const"
) + method(
    renderer, "int GfxRenderer::getScreenHeight() const"
) + method(
    renderer, "void GfxRenderer::getOrientedViewableTRBL("
) + method(
    theme, "Rect UITheme::getContentArea("
) + method(
    theme, "Rect UITheme::getScreenSafeArea("
) + method(
    theme, "Rect UITheme::getHeaderRect("
) + r"""
void UiTabListActivity::buildTabBar(UiScreen& screen) {""" + tab_bar + r"""
namespace freeink::ui {
struct GfxRendererTarget : DrawTarget { static constexpr FontId FONT_BODY = 1; };
}
struct Target : fui::GfxRendererTarget {
  std::vector<fui::Rect> textRects, fills;
  std::vector<std::string> textStrings;
  fui::Rect clip{0, 0, 32767, 32767};
  bool clippedText = false;
  fui::Rect clipRect() const override { return clip; }
  bool setClipRect(fui::Rect rect) override { clip = rect; return true; }
  fui::Size measureText(fui::FontId font, const char* text, fui::TextStyle) const override {
    EpdFont epdFont(font == 0 ? &ubuntu_10_regular : &ubuntu_12_regular);
    int width, height;
    epdFont.getTextDimensions(text, &width, &height);
    return {static_cast<int16_t>(width), lineHeight(font)};
  }
  int16_t lineHeight(fui::FontId font) const override {
    return font == 0 ? ubuntu_10_regular.advanceY : ubuntu_12_regular.advanceY;
  }
  void fill(fui::Rect rect, fui::Paint, uint8_t, uint8_t) override { fills.push_back(rect); }
  void stroke(fui::Rect, fui::Paint, uint8_t, uint8_t, uint8_t) override {}
  void line(fui::Point, fui::Point, uint8_t, fui::Paint) override {}
  void triangle(fui::Point, fui::Point, fui::Point, fui::Paint) override {}
  void text(fui::Rect rect, const char* text, fui::TextStyle) override {
    textStrings.emplace_back(text);
    if (rect.y < clip.y || rect.bottom() > clip.bottom()) clippedText = true;
    const int16_t left = std::max(rect.x, clip.x);
    const int16_t top = std::max(rect.y, clip.y);
    textRects.push_back({left, top,
                        static_cast<int16_t>(std::max(0, std::min(rect.right(), clip.right()) - left)),
                        static_cast<int16_t>(std::max(0, std::min(rect.bottom(), clip.bottom()) - top))});
  }
  void bitmap(fui::Rect, fui::BitmapRef, fui::BitmapMode, fui::Paint, fui::Rotation) override {}
};
""" + method(tokens, "inline freeink::ui::ThemeTokens uiThemeTokens(") + r"""
void checkLayout(const BoardConfig::BoardProfile& profile, GfxRenderer::Orientation orientation,
                 ThemeMetrics metrics, TextSettingsActivity::Tab tab) {
  BoardConfig::ACTIVE = profile;
  const bool touch = BoardConfig::hasTouch();
  if (touch) metrics.buttonHintsHeight = 0;
  UITheme::getInstance().metrics = metrics;
  TextSettingsActivity activity;
  activity.tab_ = tab;
  activity.renderer.panelWidth = profile.displayWidth;
  activity.renderer.panelHeight = profile.displayHeight;
  activity.renderer.orientation = orientation;
  activity.mappedInput.touch = touch;
  const int width = activity.renderer.getScreenWidth();
  const int height = activity.renderer.getScreenHeight();
  int top, right, bottom, left;
  activity.renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  const fui::Insets bezel{static_cast<int16_t>(top), static_cast<int16_t>(right),
                           static_cast<int16_t>(bottom), static_cast<int16_t>(left)};
  const Rect safe = UITheme::getInstance().getScreenSafeArea(activity.renderer, true, false);
  activity.enterGeometry();
  activity.drawChrome();
  const Rect preview = activity.renderer.preview;
  assert(preview.x == safe.x && preview.width == safe.width);
  assert(preview.y == safe.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing);
  assert(preview.y + preview.height <= safe.y + safe.height);
  assert(activity.renderer.header.x + bezel.left == safe.x);
  assert(activity.renderer.header.width == safe.width + bezel.left + bezel.right);
  assert((activity.renderer.clip == std::array<int, 4>{0, 0, 2000, 2000}));

  Target target;
  fui::DeviceContext device;
  device.width = width;
  device.height = height;
  device.safeArea = bezel;
  device.hasTouch = touch;
  fui::InputSnapshot input;
  fui::InteractionBuffer<32> interactions;
  fui::Frame<32> frame(target, device, input, interactions);
  auto theme = uiThemeTokens(target);
  for (int focus : {0, 1, 2, 3, 4, 1, 0}) {
    activity.selected = focus;
    activity.drawChrome();
    const Rect caption = activity.renderer.caption;
    assert(activity.renderer.captionText == "Preview \"Bitter, 12 pt\"");
    assert(caption.x == preview.x + metrics.previewPadding);
    assert(caption.x + caption.width <= preview.x + preview.width - metrics.previewPadding);
    assert(caption.y == preview.y + preview.height - metrics.previewPadding -
                            activity.renderer.getTextHeight(UI_10_FONT_ID));
    assert(caption.y + caption.height <= preview.y + preview.height);
    assert(activity.renderer.preview.x == preview.x && activity.renderer.preview.y == preview.y);
    assert(activity.renderer.preview.width == preview.width && activity.renderer.preview.height == preview.height);
    interactions.clear();
    target.fills.clear();
    UiScreen screen(frame, theme);
    activity.buildScreen(screen);
    const auto content = screen.contentRect();
    assert(content.x == safe.x && content.width == safe.width);
    assert(content.bottom() == safe.y + safe.height);
    assert(content.y >= preview.y + preview.height);
    assert(content.height > 0);
    assert(interactions.count() == 4);
    for (size_t i = 0; i < interactions.count(); ++i) {
      const auto tab = interactions.data()[i].rect;
      assert(tab.x >= safe.x && tab.right() <= safe.x + safe.width);
    }
    // Decoration can bleed under the bezel, but not into the button-hint strip.
    const int bandLeft = safe.x - bezel.left;
    const int bandRight = safe.x + safe.width + bezel.right;
    for (const auto& rect : target.fills) {
      assert(rect.x >= bandLeft && rect.right() <= bandRight);
    }
    assert(std::any_of(target.fills.begin(), target.fills.end(), [bandLeft, bandRight, metrics](fui::Rect rect) {
      return rect.x == bandLeft && rect.right() == bandRight && rect.height == std::max(1, metrics.headerUnderlineSize);
    }));

    // A scrolling settings list keeps values and the scroll track out of the hints.
    activity.rowItems_.assign(20, fui::ListItem{});
    for (auto& row : activity.rowItems_) { row.label = "Font"; row.value = "Selected"; }
    activity.nav.reset(1);
    target.textRects.clear();
    target.fills.clear();
    target.clippedText = false;
    activity.buildRows(screen);
    assert(!target.textRects.empty());
    for (const auto& rect : target.textRects) {
      assert(rect.x >= safe.x && rect.right() <= safe.x + safe.width);
      assert(rect.y >= content.y && rect.bottom() <= content.bottom());
    }
    assert(std::any_of(target.fills.begin(), target.fills.end(), [safe, content, theme](fui::Rect rect) {
      return rect.width == theme.listScrollWidth && rect.x >= safe.x && rect.right() <= safe.x + safe.width &&
             rect.y == content.y && rect.bottom() == content.bottom();
    }));
  }
}
void checkStyleRowsFit(const BoardConfig::BoardProfile& profile, GfxRenderer::Orientation orientation,
                       ThemeMetrics metrics) {
  BoardConfig::ACTIVE = profile;
  UITheme::getInstance().metrics = metrics;
  TextSettingsActivity activity;
  activity.tab_ = TextSettingsActivity::Tab::Style;
  activity.renderer.panelWidth = profile.displayWidth;
  activity.renderer.panelHeight = profile.displayHeight;
  activity.renderer.orientation = orientation;
  activity.enterGeometry();
  const char* labels[] = {""" + style_labels + r"""};
  for (const char* label : labels) {
    fui::ListItem row;
    row.label = label;
    row.toggle = true;
    activity.rowItems_.push_back(row);
  }

  Target target;
  fui::DeviceContext device;
  device.width = activity.renderer.getScreenWidth();
  device.height = activity.renderer.getScreenHeight();
  int top, right, bottom, left;
  activity.renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  device.safeArea = fui::Insets{static_cast<int16_t>(top), static_cast<int16_t>(right),
                                static_cast<int16_t>(bottom), static_cast<int16_t>(left)};
  fui::InputSnapshot input;
  fui::InteractionBuffer<32> interactions;
  fui::Frame<32> frame(target, device, input, interactions);
  auto theme = uiThemeTokens(target);
  for (bool checked : {false, true}) {
    for (auto& row : activity.rowItems_) row.toggleChecked = checked;
    for (int focus : {0, 1, 2, 3, 4}) {
      activity.selected = focus;
      activity.nav.reset(focus);
      interactions.clear();
      UiScreen screen(frame, theme);
      activity.buildScreen(screen);
      target.textRects.clear();
      target.textStrings.clear();
      target.clippedText = false;
      activity.buildRows(screen);
      if (activity.nav.drawnRows != 4) {
        std::fprintf(stderr, "%s orientation %d: list height %d, full rows %d\n",
                     profile.name, static_cast<int>(orientation), screen.contentRect().height, activity.nav.drawnRows);
      }
      assert(activity.nav.top == 0);
      assert(activity.nav.drawnRows == 4);
      assert(target.textRects.size() == 4);
      assert(!target.clippedText);
      assert(target.textStrings[0].find("(no preview)") == std::string::npos);
      for (int row = 0; row < 4; ++row) {
        assert(target.textStrings[row] == labels[row]);
        assert(target.measureText(theme.smallText.font, labels[row], theme.smallText).width <=
               target.textRects[row].width);
        if (row != 0) assert(target.textStrings[row].find("(no preview)") != std::string::npos);
      }
    }
  }
}
void checkSharedPartialRows() {
  Target target;
  fui::DeviceContext device;
  device.width = 800;
  device.height = 480;
  fui::InputSnapshot input;
  fui::InteractionBuffer<32> interactions;
  fui::Frame<32> frame(target, device, input, interactions);
  auto theme = fui::themeTokensForLineHeight(29);
  theme.listMinRowHeight = 40;
  TextSettingsActivity activity;
  activity.rowItems_.assign(10, fui::ListItem{});
  for (auto& row : activity.rowItems_) { row.label = "Paragraph Indentation"; row.value = "2"; }
  // Text Settings uses the shared next-row preview policy, including pixel clipping.
  for (int remainder : {4, 14, 18, 20, 24, 36}) {
    for (int selected : {1, 4}) {
      interactions.clear();
      target.textRects.clear();
      target.clippedText = false;
      activity.nav.reset(selected);
      UiScreen screen(frame, theme);
      screen.setContentMarginFromScreen(fui::Insets{100, 0, static_cast<int16_t>(260 - remainder), 0});
      const auto content = screen.contentRect();
      activity.buildRows(screen);
      const bool preview = remainder >= 18;
      assert(target.clippedText == preview);
      assert(activity.nav.drawnRows == 3);
      assert(target.textRects.size() == (preview ? 8 : 6));
      for (const auto& rect : target.textRects) {
        assert(rect.y >= content.y && rect.bottom() <= content.bottom());
      }
      assert(activity.nav.top <= selected - 1 && selected - 1 < activity.nav.top + 3);
    }
  }
}
int main() {
  using Tab = TextSettingsActivity::Tab;
  for (const auto& profile : {BoardConfig::XTEINK_X3, BoardConfig::XTEINK_X3_UC8279,
                              BoardConfig::XTEINK_X4, BoardConfig::XTEINK_X4_CLASSIC})
    for (auto orientation : {GfxRenderer::Portrait, GfxRenderer::PortraitInverted,
                             GfxRenderer::LandscapeClockwise, GfxRenderer::LandscapeCounterClockwise})
      for (const auto& metrics : {LyraMetrics::values, RoundedRaffMetrics::values})
        checkStyleRowsFit(profile, orientation, metrics);
  for (const auto& profile : {BoardConfig::XTEINK_X3, BoardConfig::XTEINK_X3_UC8279, BoardConfig::XTEINK_X4,
                              BoardConfig::XTEINK_X4_PRO, BoardConfig::XTEINK_X4_CLASSIC, BoardConfig::STICKY,
                              BoardConfig::PAPER_MONO, BoardConfig::METALIO_EINK4, BoardConfig::EEGO_A4,
                              BoardConfig::PICCO})
    for (auto orientation : {GfxRenderer::Portrait, GfxRenderer::PortraitInverted,
                             GfxRenderer::LandscapeClockwise, GfxRenderer::LandscapeCounterClockwise})
      for (const auto& metrics : {LyraMetrics::values, RoundedRaffMetrics::values})
        for (Tab tab : {Tab::Family, Tab::Size, Tab::Layout, Tab::Style})
          checkLayout(profile, orientation, metrics, tab);
  checkSharedPartialRows();
}
"""
with tempfile.TemporaryDirectory() as directory:
    cpp = Path(directory) / "layout.cpp"
    executable = Path(directory) / "layout"
    cpp.write_text(harness)
    subprocess.run([
        "c++", "-std=c++17", "-DFREEINK_DEVICE_X3=1", "-DFREEINK_DEVICE_X4=1",
        "-DFREEINK_CAP_TOUCH=1", "-I", str(ROOT / "src"),
        "-I", str(ROOT / "freeink-sdk/libs/hardware/XteinkDetect/test/host/stubs"),
        "-I", str(ROOT / "freeink-sdk/libs/hardware/BoardConfig/include"), "-I",
        str(ROOT / "freeink-sdk/libs/ui/FreeInkUI/include"),
        "-I", str(ROOT / "lib/EpdFont"), "-I", str(ROOT / "lib/Utf8"),
        str(ROOT / "freeink-sdk/libs/ui/FreeInkUI/src/FreeInkUI.cpp"),
        str(ROOT / "lib/EpdFont/EpdFont.cpp"), str(ROOT / "lib/Utf8/Utf8.cpp"),
        str(cpp), "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True)
print("Text Settings layout regression checks passed")
