#pragma once

#include <cstdint>
#include <string>
#include <vector>

class GfxRenderer;

struct LuaAppDisplayEntry {
  int16_t x = 0;
  int16_t y = 0;
  std::string text;
  bool centered = false;
};

struct LuaAppBitmapEntry {
  std::string bundleRelativePath;
  int16_t x = 0;
  int16_t y = 0;
  int16_t maxW = 0;
  int16_t maxH = 0;
};

struct LuaAppFillRectEntry {
  int16_t x = 0;
  int16_t y = 0;
  int16_t w = 0;
  int16_t h = 0;
};

struct LuaAppRectEntry {
  int16_t x = 0;
  int16_t y = 0;
  int16_t w = 0;
  int16_t h = 0;
  int16_t lineWidth = 1;
};

struct LuaAppLineEntry {
  int16_t x1 = 0;
  int16_t y1 = 0;
  int16_t x2 = 0;
  int16_t y2 = 0;
  int16_t lineWidth = 1;
};

struct LuaAppGridState {
  bool active = false;
  int16_t ox = 0;
  int16_t oy = 0;
  int16_t cell = 0;
  uint8_t board[9][9]{};
  bool givens[9][9]{};
  bool conflicts[9][9]{};
  int cursorR = 1;
  int cursorC = 1;
  bool entryMode = false;
  int entryDigit = 0;
};

struct LuaAppListState {
  bool active = false;
  std::vector<std::string> items;
  int selected = 1;
};

struct LuaAppPopupState {
  bool active = false;
  std::string message;
};

struct LuaAppChromeState {
  std::string subtitle;
  std::string hintBack;
  std::string hintConfirm;
  std::string hintLeft;
  std::string hintRight;
  bool hasHints = false;
};

// Framebuffer of draws produced during a single app run (read by AppRunnerActivity).
class LuaAppDisplay {
 public:
  static constexpr size_t kMaxEntries = 48;
  static constexpr size_t kMaxBitmapEntries = 4;
  static constexpr size_t kMaxShapeEntries = 64;
  static constexpr size_t kMaxTextLen = 96;
  static constexpr size_t kMaxBundlePathLen = 96;
  static constexpr size_t kMaxListItems = 16;
  static constexpr size_t kMaxPopupLen = 128;

  static void clear();
  static bool addText(int x, int y, const char* text, bool centered);
  static bool addBitmap(int x, int y, const char* bundleRelativePath, int maxW, int maxH);
  static bool addFillRect(int x, int y, int w, int h);
  static bool addRect(int x, int y, int w, int h, int lineWidth);
  static bool addLine(int x1, int y1, int x2, int y2, int lineWidth);
  static void setGrid(const LuaAppGridState& grid);
  static void setList(const LuaAppListState& list);
  static void setPopup(const char* message);
  static void setChrome(const LuaAppChromeState& chrome);

  static const std::vector<LuaAppDisplayEntry>& entries();
  static const std::vector<LuaAppBitmapEntry>& bitmapEntries();

  static void paintWithChrome(GfxRenderer& renderer, int fontId, const std::string& appId,
                              const std::string& displayName, int contentTop, int contentHeight);
  static void paint(GfxRenderer& renderer, int fontId, const std::string& appId, int contentTop);

 private:
  static void paintGrid(GfxRenderer& renderer, int fontId, int contentTop);
  static void paintList(GfxRenderer& renderer, int fontId, int contentTop, int contentHeight, int pageWidth);
  static void paintPopup(GfxRenderer& renderer, int fontId);

  static std::vector<LuaAppDisplayEntry> entries_;
  static std::vector<LuaAppBitmapEntry> bitmapEntries_;
  static std::vector<LuaAppFillRectEntry> fillRects_;
  static std::vector<LuaAppRectEntry> rects_;
  static std::vector<LuaAppLineEntry> lines_;
  static LuaAppGridState grid_;
  static LuaAppListState list_;
  static LuaAppPopupState popup_;
  static LuaAppChromeState chrome_;
};
