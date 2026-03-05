#pragma once

#include <string>

#include "activities/Activity.h"
#include "components/UITheme.h"

class MappedInputManager;

/**
 * Downloads a non-core language YAML from GitHub and activates it.
 *
 * Flow: WiFi selection → HTTP download → activate → finish (isCancelled=false).
 * On WiFi failure, finishes immediately with isCancelled=true.
 * On download failure, shows an error screen; Back cancels (isCancelled=true).
 */
class LanguageDownloadActivity final : public Activity {
 public:
  explicit LanguageDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const char* code,
                                    const char* name)
      : Activity("LanguageDownload", renderer, mappedInput), _code(code), _name(name) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  bool preventAutoSleep() override { return _state == DOWNLOADING; }
  bool skipLoopDelay() override { return _state == DOWNLOADING; }

 private:
  enum State { WIFI_SELECTION, DOWNLOADING, DONE, DOWNLOAD_FAILED };

  void onWifiSelectionComplete(bool connected);
  void doDownload();

  std::string _code;
  std::string _name;
  State _state = WIFI_SELECTION;
  int _progressPercent = 0;
};
