#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../Activity.h"
#include "FontInstaller.h"
#include "network/HttpDownloader.h"
#include "util/ButtonNavigator.h"

#ifndef FONT_MANIFEST_URL
#define FONT_MANIFEST_URL \
  "https://raw.githubusercontent.com/jpirnay/crosspoint-reader/master/assets/sd-fonts/fonts.json"
#endif

class FontDownloadActivity : public Activity {
 public:
  explicit FontDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state_ == LOADING_MANIFEST || state_ == DOWNLOADING; }
  bool skipLoopDelay() override { return true; }

 private:
  enum State {
    WIFI_SELECTION,
    LOADING_MANIFEST,
    FAMILY_LIST,
    DOWNLOADING,
    COMPLETE,
    ERROR,
  };

  struct ManifestFile {
    std::string name;
    size_t size = 0;
    uint32_t crc32 = 0;
    bool hasCrc32 = false;  // false = legacy v1 manifest, fall back to size-only check
  };

  struct ManifestFamily {
    std::string name;
    std::string description;
    // `styles` was once parsed here but never rendered — dropped to avoid
    // ArduinoJson string allocations that fragmented the heap before the
    // first TLS download. Resurrect if a UI surfaces style names.
    std::vector<ManifestFile> files;
    size_t totalSize = 0;
    bool installed = false;
    bool hasUpdate = false;
    // True iff a leftover __staging dir from a previous interrupted download
    // exists for this not-yet-installed family — the next confirm will resume
    // rather than restart.
    bool hasResumableDownload = false;
  };

  State state_ = WIFI_SELECTION;
  FontInstaller fontInstaller_;
  ButtonNavigator buttonNavigator_;

  // HTTP/TLS session shared across all files of a single downloadFamily()
  // call. Each family install pays the TLS handshake once (on its first
  // file); subsequent files reuse the open keep-alive connection.
  // NOT shared with the manifest fetch — holding the TLS context open
  // through the JSON parse aborts on the ~36 KB contiguous allocation
  // collision with ArduinoJson's working memory.
  HttpDownloader::Session httpSession_;

  std::string baseUrl_;
  std::vector<ManifestFamily> families_;
  int selectedIndex_ = 0;

  enum class PendingFontAction {
    None,
    Download,
    Delete,
  };

  size_t currentFileIndex_ = 0;
  size_t currentFileTotal_ = 0;
  size_t fileProgress_ = 0;
  size_t fileTotal_ = 0;
  int downloadingFamilyIndex_ = 0;
  // Cached during downloadFamily() before families_ is stashed to SD, so the
  // render path can show the family name and decide the Retry/Resume label
  // without touching families_ (which is empty during the download).
  std::string downloadingFamilyName_;
  bool downloadingFamilyHasResumable_ = false;
  PendingFontAction pendingErrorAction_ = PendingFontAction::None;
  std::string errorMessage_;
  bool cancelRequested_ = false;
  int previousActionCount_ = 0;
  int lastProgressPercent_ = -1;
  unsigned long lastProgressUpdateMs_ = 0;

  void onWifiSelectionComplete(bool success);
  bool fetchAndParseManifest();
  // Download a single family by its index into families_. Internally stashes
  // families_ to SD so the TLS handshake runs on a defragmented heap; the
  // selected family's state mutations (installed/hasUpdate/hasResumableDownload)
  // are merged back into families_ on return.
  void downloadFamily(int familyIdx);
  // Internal: the body of downloadFamily after the stash. Operates only on
  // the local family copy and constants like familyIdx; never touches
  // families_ (which is empty during this call).
  void downloadFamilyImpl(ManifestFamily& family, int familyIdx);
  void downloadAll();
  void updateAll();

  // Persist families_ to /fonts_families.bin and clear the in-memory vector.
  // Used to free the ~10 KB of scattered std::string allocations that fragment
  // the heap enough to break the TLS handshake during font downloads.
  bool stashFamiliesToSd();
  // Read /fonts_families.bin back into families_. Returns true on success.
  bool restoreFamiliesFromSd();
  bool isDownloadAllSelected() const { return hasDownloadCandidates() && selectedIndex_ == 0; }
  bool isUpdateAllSelected() const {
    if (!hasUpdateCandidates()) return false;
    return selectedIndex_ == (hasDownloadCandidates() ? 1 : 0);
  }
  bool hasDownloadCandidates() const;
  bool hasUpdateCandidates() const;
  int actionCount() const { return (hasDownloadCandidates() ? 1 : 0) + (hasUpdateCandidates() ? 1 : 0); }
  int familyIndexFromList(int listIndex) const {
    return listIndex > actionCount() - 1 ? listIndex - actionCount() : -1;
  }
  int listItemCount() const { return families_.empty() ? 0 : static_cast<int>(families_.size()) + actionCount(); }
  size_t totalUninstalledSize() const;
  size_t totalUpdateSize() const;
  void syncSelectedIndexForNewActionCount();

  std::string confirmButtonLabel() const;
  void promptDeleteFamily(int familyIndex);
  void deleteFamilyAtIndex(int familyIndex);

  static std::string formatSize(size_t bytes);
};
