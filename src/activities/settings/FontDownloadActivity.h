#pragma once

#include <string>
#include <vector>

#include "../Activity.h"
#include "FontInstaller.h"
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
  };

  struct ManifestFamily {
    std::string name;
    std::string description;
    std::vector<std::string> styles;
    std::vector<ManifestFile> files;
    size_t totalSize = 0;
    bool installed = false;
    bool hasUpdate = false;
  };

  State state_ = WIFI_SELECTION;
  FontInstaller fontInstaller_;
  ButtonNavigator buttonNavigator_;

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
  PendingFontAction pendingErrorAction_ = PendingFontAction::None;
  std::string errorMessage_;
  bool cancelRequested_ = false;

  void onWifiSelectionComplete(bool success);
  bool fetchAndParseManifest();
  void downloadFamily(ManifestFamily& family);
  void downloadAll();
  void updateAll();
  bool isDownloadAllSelected() const { return hasDownloadCandidates() && selectedIndex_ == 0; }
  bool isUpdateAllSelected() const {
    if (!hasUpdateCandidates()) return false;
    return selectedIndex_ == (hasDownloadCandidates() ? 1 : 0);
  }
  bool hasDownloadCandidates() const;
  bool hasUpdateCandidates() const;
  int actionCount() const { return (hasDownloadCandidates() ? 1 : 0) + (hasUpdateCandidates() ? 1 : 0); }
  int familyIndexFromList(int listIndex) const { return listIndex - actionCount(); }
  int listItemCount() const { return families_.empty() ? 0 : static_cast<int>(families_.size()) + actionCount(); }
  size_t totalUninstalledSize() const;
  size_t totalUpdateSize() const;

  std::string confirmButtonLabel() const;
  void promptDeleteFamily(int familyIndex);
  void deleteFamilyAtIndex(int familyIndex);

  static std::string formatSize(size_t bytes);
};
