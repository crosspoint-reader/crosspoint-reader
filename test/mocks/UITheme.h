#pragma once
// UITheme mock — components/UITheme.h

#include "GfxRenderer.h"
#include <string>
#include <functional>
#include <vector>

enum class UIIcon { Folder, Recent, Transfer, Book, Settings, Wifi, Hotspot, Library };

struct TabInfo { const char* label; bool selected; };

struct UIMetrics {
  int topPadding = 5;
  int headerHeight = 40;
  int tabBarHeight = 30;
  int verticalSpacing = 8;
  int buttonHintsHeight = 30;
  int contentSidePadding = 15;
  int progressBarHeight = 8;
  int statusBarVerticalMargin = 4;
  int listRowHeight = 35;
  int homeTopPadding = 50;
  int homeCoverTileHeight = 200;
  int homeCoverHeight = 180;
  int homeRecentBooksCount = 4;
  bool keyboardCenteredText = false;
  bool keyboardBottomAligned = true;
  int keyboardKeyWidth = 32;
  int keyboardKeyHeight = 28;
  int keyboardKeySpacing = 3;
};

struct GUIHelper {
  void drawHeader(GfxRenderer&, Rect, const char*, const char* = nullptr) {}
  void drawSubHeader(GfxRenderer&, Rect, const char*, const char* = nullptr) {}
  void drawButtonHints(GfxRenderer&, const char*, const char*, const char*, const char*) {}
  void drawSideButtonHints(GfxRenderer&, const char*, const char*) {}
  Rect drawPopup(GfxRenderer&, const char*) { return {}; }
  void fillPopupProgress(GfxRenderer&, Rect, int) {}
  void drawProgressBar(GfxRenderer&, Rect, int, int) {}
  void drawStatusBar(GfxRenderer&, float, int, int, const std::string&, int = 0, int = 0) {}
  void drawHelpText(GfxRenderer&, Rect, const char*) {}
  void drawTextField(GfxRenderer&, Rect, int) {}
  void drawKeyboardKey(GfxRenderer&, Rect, const char*, bool) {}

  // drawList with all overloads
  void drawList(GfxRenderer&, Rect, int, int,
      std::function<std::string(int)>,
      std::function<std::string(int)> = nullptr,
      std::function<UIIcon(int)> = nullptr,
      std::function<std::string(int)> = nullptr,
      bool = false) {}

  void drawTabBar(GfxRenderer&, Rect, const std::vector<TabInfo>&, bool = false) {}
  void drawRecentBookCover(GfxRenderer&, Rect, auto&, int, bool&, bool&, bool, auto) {}
  void drawButtonMenu(GfxRenderer&, Rect, int, int, std::function<std::string(int)>, std::function<UIIcon(int)>) {}
};

class UITheme {
 public:
  static UITheme& getInstance() { static UITheme t; return t; }
  const UIMetrics& getMetrics() const { return metrics; }
  const auto& getTheme() const { return *this; }
  bool showsFileIcons() const { return false; }
  void reload() {}
  uint8_t getStatusBarHeight() const { return 20; }
  uint8_t getProgressBarHeight() const { return 4; }
  int getNumberOfItemsPerPage(GfxRenderer&, bool=false, bool=false, bool=false, bool=false) { return 10; }
  static UIIcon getFileIcon(const std::string&) { return UIIcon::Book; }
  static std::string getCoverThumbPath(const std::string& p, int) { return p; }
 private:
  UIMetrics metrics;
};

// Global GUI singleton
inline GUIHelper GUI;
