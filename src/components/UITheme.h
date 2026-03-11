#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "components/themes/BaseTheme.h"

class UITheme {
 public:
  UITheme();
  static UITheme& getInstance();  // Meyers singleton - lazy initialization
  
  void ensureInitialized();  // Called on first use after settings loaded

  const ThemeMetrics& getMetrics() const {
    const_cast<UITheme*>(this)->ensureInitialized();
    return *currentMetrics;
  }
  const BaseTheme& getTheme() const {
    const_cast<UITheme*>(this)->ensureInitialized();
    return *currentTheme;
  }
  void reload();
  void setTheme(CrossPointSettings::UI_THEME type);
  static int getNumberOfItemsPerPage(const GfxRenderer& renderer, bool hasHeader, bool hasTabBar, bool hasButtonHints,
                                     bool hasSubtitle);
  static std::string getCoverThumbPath(std::string coverBmpPath, int coverHeight);
  static UIIcon getFileIcon(const std::string& filename);
  static int getStatusBarHeight();
  static int getProgressBarHeight();

  // Network connectivity status — used by themes to draw indicators (e.g. PULSR left-bar segment).
  static void setNetworkStatus(bool connected, bool transferring);
  static bool isNetworkConnected();
  static bool isNetworkTransferring();
  static void setHttpServerActive(bool active);
  static bool isHttpServerActive();
  static void setWifiAutoConnecting(bool connecting);
  static bool isWifiAutoConnecting();
  static void setUsbConnected(bool connected);
  static bool isUsbConnected();
  static void addReceivedFile(const std::string& name);
  static const std::vector<std::string>& getReceivedFiles();
  static void clearReceivedFiles();
  // Returns true (once) if a new file arrived since the last call.
  static bool consumeReceivedFileDirty();
  // Returns true if the active theme renders with inverted (dark) colours.
  static bool isInverted();

 private:
  const ThemeMetrics* currentMetrics;
  std::unique_ptr<BaseTheme> currentTheme;
};

// Helper macro to access current theme
#define GUI UITheme::getInstance().getTheme()
