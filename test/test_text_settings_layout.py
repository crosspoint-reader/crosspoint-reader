"""Run with python3 test/test_text_settings_layout.py (requires a host C++ compiler)."""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
activity = (ROOT / "src/activities/settings/TextSettingsActivity.cpp").read_text()
tabs = (ROOT / "src/activities/UiTabListActivity.cpp").read_text()
lists = (ROOT / "src/activities/UiListActivity.cpp").read_text()
renderer = (ROOT / "lib/GfxRenderer/GfxRenderer.cpp").read_text()
theme = (ROOT / "src/components/UITheme.cpp").read_text()
tokens = (ROOT / "src/components/UIThemeTokens.h").read_text()


def method(source, signature):
    return signature + source.split(signature, 1)[1].split("\n}", 1)[0] + "\n}\n"


# Exercise production geometry with the real SDK screen and tab components,
# without loading fonts, SD storage, or the activity manager.
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
caption = activity.split("void TextSettingsActivity::drawFooter() {", 1)[1]
caption = caption.split("\n  const auto labels", 1)[0]
orientation = (ROOT / "lib/GfxRenderer/GfxRenderer.h").read_text()
orientation = "enum Orientation {" + orientation.split("enum Orientation {", 1)[1].split("\n  };", 1)[0] + "\n  };"

harness = r"""
#include <FreeInkApp.h>
#include <BoardConfig.h>
#include <components/themes/lyra/LyraTheme.h>
#include <components/themes/roundedraff/RoundedRaffTheme.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <string>
#include <vector>
namespace fui = freeink::ui;
using UiScreen = fui::Screen<32>;
struct GfxRenderer {
""" + orientation + r"""
  Orientation orientation = Portrait;
  int panelWidth = 800, panelHeight = 480;
  Rect preview{}, header{}, caption{};
  mutable std::array<int, 4> clip{0, 0, 2000, 2000};
  int getScreenWidth() const;
  int getScreenHeight() const;
  Orientation getOrientation() const { return orientation; }
  void getOrientedViewableTRBL(int*, int*, int*, int*) const;
  int getTextHeight(int) const { return 20; }
  void drawText(int, int x, int y, const char*) { caption = Rect{x, y, 40, 24}; }
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
#define tr(id) "Text Settings"
constexpr int UI_10_FONT_ID = 0, TOUCH_TAB_BAR_HEIGHT = 50, ACTION_TAB = 1, ACTION_ROW = 2;
namespace textsettings {
  struct PreviewLayout {};
  void renderPreview(GfxRenderer& renderer, PreviewLayout&, int, int, Rect bounds, const char*, const char*) {
    renderer.preview = bounds;
    assert((renderer.clip == std::array<int, 4>{bounds.x, bounds.y, bounds.width, bounds.height}));
  }
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
  int afterHeader = 0, usableHeight = 0, previewHeight = 0;
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
  bool focusedRowHasNoPreview() const { return tab_ == Tab::Style && selected >= 2; }
  void drawCaption() {""" + caption + r"""
  }
};
""" + sync + method(renderer, "int GfxRenderer::getScreenWidth() const") + method(
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
  fui::Rect clip{0, 0, 32767, 32767};
  bool clippedText = false;
  fui::Rect clipRect() const override { return clip; }
  bool setClipRect(fui::Rect rect) override { clip = rect; return true; }
  fui::Size measureText(fui::FontId font, const char*, fui::TextStyle) const override { return {40, lineHeight(font)}; }
  int16_t lineHeight(fui::FontId font) const override { return font == 0 ? 24 : 29; }
  void fill(fui::Rect rect, fui::Paint, uint8_t, uint8_t) override { fills.push_back(rect); }
  void stroke(fui::Rect, fui::Paint, uint8_t, uint8_t, uint8_t) override {}
  void line(fui::Point, fui::Point, uint8_t, fui::Paint) override {}
  void triangle(fui::Point, fui::Point, fui::Point, fui::Paint) override {}
  void text(fui::Rect rect, const char*, fui::TextStyle) override {
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
  for (int focus : {0, 1, 2}) {
    activity.selected = focus;
    interactions.clear();
    target.fills.clear();
    UiScreen screen(frame, theme);
    activity.buildScreen(screen);
    const auto content = screen.contentRect();
    assert(content.x == safe.x && content.width == safe.width);
    const int footerHeight = tab == TextSettingsActivity::Tab::Style
                                 ? activity.renderer.getTextHeight(UI_10_FONT_ID) + 2 * metrics.verticalSpacing
                                 : 0;
    assert(content.bottom() == safe.y + safe.height - footerHeight);
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

    activity.renderer.caption = Rect{};
    activity.drawCaption();
    if (activity.focusedRowHasNoPreview()) {
      const Rect caption = activity.renderer.caption;
      assert(caption.y == content.bottom() + metrics.verticalSpacing);
      assert(caption.y + caption.height <= safe.y + safe.height);
    }

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
        str(ROOT / "freeink-sdk/libs/ui/FreeInkUI/src/FreeInkUI.cpp"),
        str(cpp), "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True)
print("Text Settings layout regression checks passed")
