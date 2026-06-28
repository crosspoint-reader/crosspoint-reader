#pragma once

#include <string>
#include <vector>

#include "ThemeInstaller.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

#define THEMES_MANIFEST_VERSION 1
#define THEME_ROOT_URL "http://crosspointreader.com/themes"

#ifndef THEME_MANIFEST_URL
#define THEME_MANIFEST_URL THEME_ROOT_URL "/themes.json"
#endif

class ThemeDownloadActivity : public Activity {
 public:
  explicit ThemeDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override {
    return state_ == LOADING_MANIFEST || state_ == DOWNLOADING || state_ == COMPLETE || state_ == ERROR;
  }
  bool skipLoopDelay() override { return true; }

 private:
  enum State {
    WIFI_SELECTION,
    LOADING_MANIFEST,
    THEME_LIST,
    DOWNLOADING,
    COMPLETE,
    ERROR,
  };

  struct ManifestFile {
    std::string path;
    std::string url;
    size_t size = 0;
    uint32_t crc32 = 0;
  };

  struct ManifestTheme {
    std::string id;
    std::string name;
    std::string description;
    int version = 0;
    std::vector<ManifestFile> files;
    size_t totalSize = 0;
    bool installed = false;
    bool hasUpdate = false;
  };

  State state_ = WIFI_SELECTION;
  ThemeInstaller themeInstaller_;
  ButtonNavigator buttonNavigator_;

  std::string baseUrl_;
  std::vector<ManifestTheme> themes_;
  int selectedIndex_ = 0;

  size_t currentFileIndex_ = 0;
  size_t currentFileTotal_ = 0;
  size_t fileProgress_ = 0;
  size_t fileTotal_ = 0;
  int downloadingThemeIndex_ = -1;
  std::string errorMessage_;
  bool cancelRequested_ = false;

  void onWifiSelectionComplete(bool success);
  bool fetchAndParseManifest();
  void downloadTheme(ManifestTheme& theme);
  void downloadAll();
  void updateAll();
  static bool computeFileCrc32(const char* path, uint32_t& outCrc);
  bool showDownloadAllRow() const;
  bool showUpdateAllRow() const;
  int specialRowCount() const;
  bool isDownloadAllRow(int index) const;
  bool isUpdateAllRow(int index) const;
  bool isSelectedThemeDeletable() const;
  void promptDeleteSelectedTheme();
  void onDeleteConfirmationResult(const ActivityResult& result);
  int themeIndexFromList(int listIndex) const { return listIndex - specialRowCount(); }
  int listItemCount() const;
  size_t totalDownloadSize() const;
  size_t totalUpdateSize() const;
  static std::string formatSize(size_t bytes);
};
