#include "UITheme.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <Logging.h>

#include <atomic>
#include <memory>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/themes/BaseTheme.h"
#include "components/themes/lyra/Lyra3CoversTheme.h"
#include "components/themes/lyra/LyraTheme.h"
#include "components/themes/pulsr/DarkPulsrTheme.h"
#include "components/themes/pulsr/PulsrTheme.h"

namespace {
constexpr int SKIP_PAGE_MS = 700;
}  // namespace

// Lazy initialization - delay theme creation until settings are loaded
// Uses Meyer's singleton pattern for thread-safe initialization
UITheme& UITheme::getInstance() {
  static UITheme instance;
  return instance;
}

// Thread-safe status flags using std::atomic<bool>
static std::atomic<bool> s_networkConnected{false};
static std::atomic<bool> s_networkTransferring{false};

// setNetworkStatus / isNetworkConnected / isNetworkTransferring track WiFi/transfer state.
// Currently called by CrossPointWebServerActivity; isNetworkTransferring() drives the
// PULSR HTTP pill brightness (white = active transfer, gray = idle).
// isNetworkConnected() is available for future use (e.g. persistent WiFi indicators).
// Uses memory_order_relaxed for lock-free performance (these are best-effort indicators).
void UITheme::setNetworkStatus(bool connected, bool transferring) {
  s_networkConnected.store(connected, std::memory_order_relaxed);
  s_networkTransferring.store(transferring, std::memory_order_relaxed);
}
bool UITheme::isNetworkConnected() { return s_networkConnected.load(std::memory_order_relaxed); }
bool UITheme::isNetworkTransferring() { return s_networkTransferring.load(std::memory_order_relaxed); }

UITheme::UITheme() : currentTheme(nullptr), currentMetrics(nullptr) {
  // Theme will be initialized on first use via reload()
}

void UITheme::ensureInitialized() {
  if (!currentTheme) {
    reload();
  }
}

void UITheme::reload() {
  auto themeType = static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme);
  setTheme(themeType);
}

void UITheme::setTheme(CrossPointSettings::UI_THEME type) {
  switch (type) {
    case CrossPointSettings::UI_THEME::CLASSIC:
      LOG_DBG("UI", "Using Classic theme");
      currentTheme = std::make_unique<BaseTheme>();
      currentMetrics = &BaseMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::LYRA:
      LOG_DBG("UI", "Using Lyra theme");
      currentTheme = std::make_unique<LyraTheme>();
      currentMetrics = &LyraMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::LYRA_3_COVERS:
      LOG_DBG("UI", "Using Lyra 3 Covers theme");
      currentTheme = std::make_unique<Lyra3CoversTheme>();
      currentMetrics = &Lyra3CoversMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::PULSR:
      LOG_DBG("UI", "Using PULSR theme");
      currentTheme = std::make_unique<PulsrTheme>();
      currentMetrics = &PulsrMetrics::values;
      break;
    case CrossPointSettings::UI_THEME::DARK_PULSR:
      LOG_DBG("UI", "Using Dark PULSR theme");
      currentTheme = std::make_unique<DarkPulsrTheme>();
      currentMetrics = &PulsrMetrics::values;
      break;
    default:
      LOG_ERR("UI", "Unknown theme %d, falling back to Classic", static_cast<int>(type));
      currentTheme = std::make_unique<BaseTheme>();
      currentMetrics = &BaseMetrics::values;
      break;
  }
}

int UITheme::getNumberOfItemsPerPage(const GfxRenderer& renderer, bool hasHeader, bool hasTabBar, bool hasButtonHints,
                                     bool hasSubtitle) {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  int reservedHeight = metrics.topPadding;
  if (hasHeader) {
    reservedHeight += metrics.headerHeight + metrics.verticalSpacing;
  }
  if (hasTabBar) {
    reservedHeight += metrics.tabBarHeight;
  }
  if (hasButtonHints) {
    reservedHeight += metrics.verticalSpacing + metrics.buttonHintsHeight;
  }
  const int availableHeight = renderer.getScreenHeight() - reservedHeight;
  int rowHeight = hasSubtitle ? metrics.listWithSubtitleRowHeight : metrics.listRowHeight;
  return availableHeight / rowHeight;
}

std::string UITheme::getCoverThumbPath(std::string coverBmpPath, int coverHeight) {
  size_t pos = coverBmpPath.find("[HEIGHT]", 0);
  if (pos != std::string::npos) {
    coverBmpPath.replace(pos, 8, std::to_string(coverHeight));
  }
  return coverBmpPath;
}

UIIcon UITheme::getFileIcon(const std::string& filename) {
  if (filename.empty()) {
    return File;
  }
  if (filename.back() == '/') {
    return Folder;
  }
  if (FsHelpers::hasEpubExtension(filename) || FsHelpers::hasXtcExtension(filename)) {
    return Book;
  }
  if (FsHelpers::hasTxtExtension(filename) || FsHelpers::hasMarkdownExtension(filename)) {
    return Text;
  }
  if (FsHelpers::hasBmpExtension(filename)) {
    return Image;
  }
  return File;
}

int UITheme::getStatusBarHeight() {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();

  // Add status bar margin
  const bool showStatusBar = SETTINGS.statusBarChapterPageCount || SETTINGS.statusBarBookProgressPercentage ||
                             SETTINGS.statusBarTitle != CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE ||
                             SETTINGS.statusBarBattery;
  const bool showProgressBar =
      SETTINGS.statusBarProgressBar != CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS;
  return (showStatusBar ? (metrics.statusBarVerticalMargin) : 0) +
         (showProgressBar ? (((SETTINGS.statusBarProgressBarThickness + 1) * 2) + metrics.progressBarMarginTop) : 0);
}

int UITheme::getProgressBarHeight() {
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  const bool showProgressBar =
      SETTINGS.statusBarProgressBar != CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS;
  return (showProgressBar ? (((SETTINGS.statusBarProgressBarThickness + 1) * 2) + metrics.progressBarMarginTop) : 0);
}

// Thread-safe status flags for HTTP server, WiFi, and USB connectivity
static std::atomic<bool> s_httpServerActive{false};
static std::atomic<bool> s_wifiAutoConnecting{false};
static std::atomic<bool> s_usbConnected{false};

void UITheme::setHttpServerActive(bool active) { s_httpServerActive.store(active, std::memory_order_relaxed); }
bool UITheme::isHttpServerActive() { return s_httpServerActive.load(std::memory_order_relaxed); }
void UITheme::setWifiAutoConnecting(bool connecting) { s_wifiAutoConnecting.store(connecting, std::memory_order_relaxed); }
bool UITheme::isWifiAutoConnecting() { return s_wifiAutoConnecting.load(std::memory_order_relaxed); }
void UITheme::setUsbConnected(bool connected) { s_usbConnected.store(connected, std::memory_order_relaxed); }
bool UITheme::isUsbConnected() { return s_usbConnected.load(std::memory_order_relaxed); }

// Received files tracking — note: vector access is NOT synchronized
// Current usage pattern assumes single-threaded UI updates
static std::vector<std::string> s_receivedFiles;
static std::atomic<bool> s_receivedFileDirty{false};
static constexpr size_t MAX_RECEIVED_FILES = 12;
static constexpr size_t MAX_FILENAME_LEN = 80;

void UITheme::addReceivedFile(const std::string& name) {
  if (s_receivedFiles.size() >= MAX_RECEIVED_FILES) {
    s_receivedFiles.erase(s_receivedFiles.begin());
  }
  const std::string truncated = (name.size() <= MAX_FILENAME_LEN) ? name : name.substr(0, MAX_FILENAME_LEN - 3) + "...";
  s_receivedFiles.push_back(std::move(truncated));
  s_receivedFileDirty.store(true, std::memory_order_release);
}

const std::vector<std::string>& UITheme::getReceivedFiles() { return s_receivedFiles; }

void UITheme::clearReceivedFiles() {
  s_receivedFiles.clear();
  s_receivedFileDirty.store(false, std::memory_order_release);
}

bool UITheme::consumeReceivedFileDirty() {
  const bool dirty = s_receivedFileDirty.load(std::memory_order_acquire);
  s_receivedFileDirty.store(false, std::memory_order_release);
  return dirty;
}
bool UITheme::isInverted() {
  const auto& inst = getInstance();
  return inst.currentTheme && inst.currentTheme->isInverted();
}
