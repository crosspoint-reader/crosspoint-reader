#pragma once

#include "activities/Activity.h"
#include "components/UITheme.h"

class MappedInputManager;

/**
 * Fetches the non-core language manifest from GitHub and caches it on the SD
 * card at /.crosspoint/languages/manifest.json.
 *
 * Flow: WiFi selection → HTTP download → finish (isCancelled=false on success).
 */
class LanguageManifestFetchActivity final : public Activity {
 public:
  explicit LanguageManifestFetchActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("LanguageManifestFetch", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  bool preventAutoSleep() override { return _state == FETCHING; }
  bool skipLoopDelay() override { return _state == FETCHING; }

 private:
  enum State { WIFI_SELECTION, FETCHING, DONE, FAILED };

  void onWifiSelectionComplete(bool connected);
  void doFetch();

  State _state = WIFI_SELECTION;
};
