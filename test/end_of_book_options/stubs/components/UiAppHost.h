#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace marquee_test {
inline uint32_t now = 0;
inline std::vector<std::string> books;
}  // namespace marquee_test

inline unsigned long millis() { return marquee_test::now; }

namespace freeink::ui {
using ActionId = int;
struct Rect {
  int16_t x = 0, y = 0, width = 160, height = 300;
};
struct Insets {
  int16_t top, right, bottom, left;
};
struct TextStyle {
  int font = 0;
  int maxLines = 1;
};
struct Size {
  int width;
};
struct DrawTarget {
  Size measureText(int, const char* value, TextStyle) const {
    int count = 0;
    for (const auto* p = reinterpret_cast<const unsigned char*>(value); *p; ++p) {
      if ((*p & 0xC0) != 0x80) ++count;
    }
    return {count * 8};
  }
};
struct ListItem {
  const char* label = "";
  int16_t actionValue = 0;
};
struct ListProps {
  const ListItem* items = nullptr;
  uint16_t count = 0;
  int16_t selectedIndex = 0;
  ActionId action = 0;
  int inputMask = 0;
  TextStyle labelText;
  int16_t rowHeight = 0, rowGap = -1, rowInset = -1;
  int16_t scrollIndicatorWidth = -1, scrollIndicatorInset = -1, sidePadding = -1;
  bool scrollIndicator = true;
};
struct ActionEvent {
  int16_t value = -1;
};
constexpr int InputTouch = 1;
inline uint16_t listVisibleRows(Rect rect, int16_t height, int16_t gap) {
  if (rect.height <= 0 || height <= 0) return 0;
  gap = std::max<int16_t>(gap, 0);
  return static_cast<uint16_t>((rect.height + gap) / (height + gap));
}
}  // namespace freeink::ui

using Rect = freeink::ui::Rect;
struct GfxRenderer {
  int width = 160, height = 300;
  int getScreenWidth() const { return width; }
  int getScreenHeight() const { return height; }
  int getLineHeight(int) const { return 12; }
  template <typename... Args>
  void drawCenteredText(Args&&...) {}
};
struct MappedInputManager {
  enum class Button { Confirm, Back, NavPrevious, NavNext };
  int pressed = -1, released = -1, tappedRow = -1;
  unsigned long held = 0;
  bool wasPressed(Button b) const { return pressed == static_cast<int>(b); }
  bool wasReleased(Button b) const { return released == static_cast<int>(b); }
  unsigned long getHeldTime() const { return held; }
  struct Labels {
    const char *btn1, *btn2, *btn3, *btn4;
  };
  Labels mapLabels(const char* a, const char* b, const char* c, const char* d) const { return {a, b, c, d}; }
};
struct ScreenTheme {
  freeink::ui::TextStyle bodyText;
  int16_t rowHeight = 20, listRowGap = 0, listInset = 0;
  int16_t listScrollWidth = 4, listScrollInset = 2, listSidePadding = 8;
};
struct UiScreen {
  freeink::ui::DrawTarget drawTarget;
  ScreenTheme screenTheme;
  Rect content;
  std::vector<std::string> labels;
  int selected = 0;
  void setContentMarginFromScreen(freeink::ui::Insets) {}
  const ScreenTheme& theme() const { return screenTheme; }
  const freeink::ui::DrawTarget& target() const { return drawTarget; }
  Rect body() const { return content; }
  void list(const freeink::ui::ListProps& props) {
    selected = props.selectedIndex;
    labels.clear();
    for (uint16_t i = 0; i < props.count; ++i) labels.emplace_back(props.items[i].label);
  }
};
namespace marquee_test {
inline UiScreen screen;
}

class UiAppHost {
 public:
  using UiScreen = ::UiScreen;
  struct App {
    void (*screenFn)(UiScreen&, void*) = nullptr;
    void* screenUser = nullptr;
    void (*rowFn)(const freeink::ui::ActionEvent&, void*) = nullptr;
    void* rowUser = nullptr;
    void on(freeink::ui::ActionId, void (*fn)(const freeink::ui::ActionEvent&, void*), void* user) {
      rowFn = fn;
      rowUser = user;
    }
    void setScreen(void (*fn)(UiScreen&, void*), void* user) {
      screenFn = fn;
      screenUser = user;
    }
    void clearTapFlash() {}
    bool invalidated() const { return false; }
  } app;
  explicit UiAppHost(const GfxRenderer&) {}
  void resetUi() {}
  void renderUi() { app.screenFn(marquee_test::screen, app.screenUser); }
  struct TouchRoute {
    bool routed = false;
    explicit operator bool() const { return routed; }
  };
  TouchRoute routeTouch(const MappedInputManager& input) {
    if (input.tappedRow < 0) return {};
    app.rowFn({static_cast<int16_t>(input.tappedRow)}, app.rowUser);
    return {true};
  }
};

struct ThemeMetrics {
  int verticalSpacing = 4, listRowHeight = 20;
};
struct UITheme {
  static UITheme& getInstance() {
    static UITheme theme;
    return theme;
  }
  const ThemeMetrics& getMetrics() const {
    static ThemeMetrics metrics;
    return metrics;
  }
  static Rect getScreenSafeArea(GfxRenderer& renderer, bool, bool) {
    return {0, 0, static_cast<int16_t>(renderer.width), static_cast<int16_t>(renderer.height)};
  }
  template <typename... Args>
  static void drawCenteredText(Args&&...) {}
  template <typename... Args>
  void drawButtonHints(Args&&...) {}
};
#define GUI UITheme::getInstance()
struct Gpio {
  bool touch = true;
  bool hasTouch() const { return touch; }
};
inline Gpio gpio;
struct CrossPointSettings {
  static constexpr int OFF = 0;
  int longPressButtonBehavior = OFF;
  int getRefreshFrequency() const { return 1; }
};
inline CrossPointSettings SETTINGS;
namespace ReaderUtils {
constexpr unsigned long GO_HOME_MS = 1000;
}
namespace FsHelpers {
inline std::string extractFolderPath(const std::string& path) { return path.substr(0, path.rfind('/')); }
}  // namespace FsHelpers
namespace NextBookFinder {
inline std::vector<std::string> findNextBooks(const std::string&, size_t) { return marquee_test::books; }
}  // namespace NextBookFinder
struct ButtonNavigator {
  static int previousIndex(int index, int count) { return (index + count - 1) % count; }
  static int nextIndex(int index, int count) { return (index + 1) % count; }
};
namespace EpdFontFamily {
constexpr int BOLD = 1;
}
constexpr int UI_12_FONT_ID = 12, UI_10_FONT_ID = 10;
constexpr const char *STR_EOB_HOME = "Home", *STR_END_OF_BOOK = "End of book", *STR_EOB_CONTINUE_WITH = "Continue with";
constexpr const char *STR_BACK = "Back", *STR_OPEN = "Open", *STR_DIR_UP = "Up", *STR_DIR_DOWN = "Down";
inline const char* tr(const char* text) { return text; }
