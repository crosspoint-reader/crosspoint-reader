#pragma once
#include <string>
#include <vector>

#include "../Activity.h"
#include "util/ButtonNavigator.h"

/**
 * A basic text-mode web browser.
 *
 * Fetches a URL over WiFi, strips HTML tags to produce readable plain text,
 * and displays the result as paginated text on the e-ink screen.
 *
 * Navigation:
 *   Up / Down / Left / Right  → previous / next page
 *   Confirm                   → enter a new URL
 *   Back                      → previous URL in history, or go home
 */
class TextBrowserActivity final : public Activity {
 public:
  enum class State { CHECK_WIFI, WIFI_SELECTION, URL_INPUT, LOADING, BROWSING, ERROR };

  static constexpr int MAX_HISTORY = 10;
  static constexpr size_t MAX_HTML_BYTES = 65536;  // 64 KB cap on raw HTML

  explicit TextBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("TextBrowser", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  State state = State::CHECK_WIFI;

  std::string currentUrl;
  std::string pageTitle;
  std::string statusMessage;
  std::string errorMessage;

  // All word-wrapped lines of the fetched page content
  std::vector<std::string> lines;
  int currentPage = 0;
  int totalPages = 0;
  int linesPerPage = 0;

  // Browsed URL history (most-recent last)
  std::vector<std::string> urlHistory;

  // Suppress duplicate button-release events when returning from a sub-activity
  bool consumeConfirm = false;
  bool consumeBack = false;

  ButtonNavigator buttonNavigator;

  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void launchUrlInput(const std::string& initial = "https://");
  void fetchAndProcess(const std::string& url);
  void buildPages(const std::string& text);
  bool preventAutoSleep() override { return true; }
};
