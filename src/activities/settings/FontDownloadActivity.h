#pragma once

#include <string>
#include <vector>

#include "FontInstaller.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

#ifndef FONT_MANIFEST_URL
#define FONT_MANIFEST_URL "https://github.com/crosspoint-reader/crosspoint-reader/releases/download/sd-fonts/fonts.json"
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
    CONFIRM_DOWNLOAD,
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

  // Manifest data
  std::string baseUrl_;
  std::vector<ManifestFamily> families_;
  int selectedIndex_ = 0;

  // Download progress
  size_t currentFileIndex_ = 0;
  size_t currentFileTotal_ = 0;
  size_t fileProgress_ = 0;
  size_t fileTotal_ = 0;
  int downloadingFamilyIndex_ = 0;
  std::string errorMessage_;

  void onWifiSelectionComplete(bool success);
  bool fetchAndParseManifest();
  void downloadFamily(ManifestFamily& family);
  void downloadAll();
  bool isDownloadAllSelected() const { return selectedIndex_ == 0 && !families_.empty(); }
  int familyIndexFromList(int listIndex) const { return listIndex - 1; }
  int listItemCount() const { return families_.empty() ? 0 : static_cast<int>(families_.size()) + 1; }
  size_t totalUninstalledSize() const;
  static std::string formatSize(size_t bytes);
};
