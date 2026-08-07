#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"

/**
 * One-button upload of the reader's highlight files to Koofr over WebDAV.
 *
 * Flow:
 * 1. Check Koofr credentials are configured
 * 2. Collect the highlight files on the SD card (/Highlights/*.md and, in
 *    single-file mode, /Highlights.md)
 * 3. Connect to WiFi (if not connected)
 * 4. Create the remote folder, then PUT each file
 * 5. Show the tally and return home
 *
 * Every file is re-uploaded on every run: highlight markdown is a few KB, and
 * tracking per-file state on the SD card would cost more than the transfer it
 * saves.
 */
class HighlightSyncActivity final : public Activity {
 public:
  explicit HighlightSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("HighlightSync", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == UPLOADING; }

 private:
  enum State : uint8_t {
    NO_CREDENTIALS,
    NO_HIGHLIGHTS,
    UPLOADING,
    DONE,
    FAILED,
  };

  // One highlight file queued for upload. Both fields are needed because the
  // single-file mode's source path (/Highlights.md) is not inside the folder
  // the per-book files come from.
  struct HighlightFile {
    std::string path;  // absolute path on the SD card
    std::string name;  // file name to write remotely
  };

  State state = UPLOADING;
  std::string statusMessage;
  std::vector<HighlightFile> files;

  int uploadedCount = 0;
  int failedCount = 0;

  // Set past the credentials check, when WiFi is about to be brought up.
  // Checked in onExit to decide whether to silent-reboot, which WiFi.getMode()
  // cannot answer reliably once the radio has been torn down.
  bool wifiActivated = false;

  // Fills `files` from the SD card. Returns the number found.
  size_t collectHighlightFiles();
  void onWifiSelectionComplete(bool success);
  void performUpload();
  void setStatus(State newState, std::string message);
};
