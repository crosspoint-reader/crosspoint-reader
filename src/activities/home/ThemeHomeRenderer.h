#pragma once

#include <vector>

#include "components/themes/ThemeLayout.h"

class GfxRenderer;
class MappedInputManager;
struct RecentBook;

struct ThemeHomeActionEntry {
  ThemeHomeAction action = ThemeHomeAction::FileBrowser;
  int value = 0;
};

using ThemeHomeBufferCallback = bool (*)(void*);

void buildThemeHomeActions(const ThemeHomeScreenSpec* spec, const std::vector<RecentBook>& recentBooks,
                           bool hasOpdsServers, std::vector<ThemeHomeActionEntry>& actions);

struct ThemeHomeRenderContext {
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;
  const ThemeMetrics& metrics;
  const ThemeHomeScreenSpec& spec;
  const std::vector<RecentBook>& recentBooks;
  const std::vector<ThemeHomeActionEntry>& actions;
  bool hasOpdsServers = false;
  int selectorIndex = 0;
  int& coverSelectorIndex;
  bool& coverRendered;
  bool& coverBufferStored;
  int coverBufferSelectorIndex = -1;
  bool coverBufferStripSelected = false;
  int& coverRectX;
  int& coverRectY;
  int& coverRectW;
  int& coverRectH;
  void* coverBufferUserData = nullptr;
  ThemeHomeBufferCallback storeCoverBuffer = nullptr;
  ThemeHomeBufferCallback restoreCoverBuffer = nullptr;
};

bool renderThemeHome(ThemeHomeRenderContext& ctx);
