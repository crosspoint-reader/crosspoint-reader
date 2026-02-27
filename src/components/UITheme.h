#include <vector>
#include <string>
#pragma once

#include <functional>
#include <memory>

#include "CrossPointSettings.h"
#include "components/themes/BaseTheme.h"

class UITheme {
  // Static instance
  static UITheme instance;

 public:
  UITheme();
  static UITheme& getInstance() { return instance; }

  const ThemeMetrics& getMetrics() const { return *currentMetrics; }
  const BaseTheme& getTheme() const { return *currentTheme; }
  void reload();
  void setTheme(CrossPointSettings::UI_THEME type);
  static int getNumberOfItemsPerPage(const GfxRenderer& renderer, bool hasHeader, bool hasTabBar, bool hasButtonHints,
                                     bool hasSubtitle);
  static std::string getCoverThumbPath(std::string coverBmpPath, int coverHeight);
  static UIIcon getFileIcon(std::string filename);
  static int getStatusBarHeight();
  static int getProgressBarHeight();

  // Network connectivity status — used by themes to draw indicators (e.g. PULSR left-bar segment).
  static void setNetworkStatus(bool connected, bool transferring);
  static bool isNetworkConnected();
  static bool isNetworkTransferring();
  static void setHttpServerActive(bool active);
  static bool isHttpServerActive();

  // Shared received-files list (HTTP uploads + feed downloads)
  static void addReceivedFile(const std::string& name);
  static const std::vector<std::string>& getReceivedFiles();
  static void clearReceivedFiles();
  static bool consumeReceivedFileDirty();  // returns true (and clears) if a new file was added since last call

 private:
  const ThemeMetrics* currentMetrics;
  std::unique_ptr<BaseTheme> currentTheme;
};

// Helper macro to access current theme
#define GUI UITheme::getInstance().getTheme()
