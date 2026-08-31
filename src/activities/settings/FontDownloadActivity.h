#pragma once

#include <string>
#include <vector>

#include "FontInstaller.h"
#include "SdCardFont.h"
#include "activities/UiListActivity.h"

// JSON schema version of the fonts.json manifest. The canonical version for
// the build tooling lives in lib/EpdFont/scripts/cpfont_version.py. This
// firmware-side copy must be bumped manually when the firmware is updated to
// support a new manifest schema.
#define FONTS_MANIFEST_VERSION 1

#ifndef FONT_MANIFEST_URL
// Manifest + .cpfont assets are published by .github/workflows/release-fonts.yml
// to the crosspoint-fonts repo under the "sd-fonts-m<META>-b<BIN>" tag. The tag
// pattern must stay in sync with the workflow; it derives its version numbers
// from lib/EpdFont/scripts/cpfont_version.py.
#define FONT_MANIFEST_URL_STRINGIFY_INNER(x) #x
#define FONT_MANIFEST_URL_STRINGIFY(x) FONT_MANIFEST_URL_STRINGIFY_INNER(x)
#define FONT_MANIFEST_URL                                                                                           \
  "https://github.com/crosspoint-reader/crosspoint-fonts/releases/download/sd-fonts-m" FONT_MANIFEST_URL_STRINGIFY( \
      FONTS_MANIFEST_VERSION) "-b" FONT_MANIFEST_URL_STRINGIFY(CPFONT_VERSION) "/fonts.json"
#endif

class FontDownloadActivity final : public UiListActivity {
 public:
  explicit FontDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override {
    return state_ == LOADING_MANIFEST || state_ == DOWNLOADING ||
           // The download is synchronous and blocks the main loop until it
           // completes, so activityManager.preventAutoSleep() is never polled
           // during downloading.
           state_ == COMPLETE || state_ == ERROR;
  }
  bool skipLoopDelay() override { return true; }

 private:
  enum State {
    WIFI_SELECTION,
    LOADING_MANIFEST,
    GROUP_LIST,
    FAMILY_LIST,
    DOWNLOADING,
    COMPLETE,
    ERROR,
  };

  struct ManifestFile {
    std::string name;
    size_t size = 0;
    uint32_t crc32 = 0;
  };

  struct ManifestFamily {
    std::string name;
    std::string description;
    std::vector<std::string> styles;
    std::vector<ManifestFile> files;
    size_t totalSize = 0;
    bool installed = false;
    bool hasUpdate = false;
    uint32_t scriptMask = 0;
  };

  static constexpr size_t MAX_SCRIPT_GROUPS = 32;

  State state_ = WIFI_SELECTION;
  FontInstaller fontInstaller_;

  // Manifest data
  std::string baseUrl_;
  std::vector<ManifestFamily> families_;
  // Manifest-defined labels are dynamic; cap them at the 32-bit membership
  // mask and retain only labels after parsing so group tags consume no steady-state heap.
  std::vector<std::string> scriptGroupLabels_;
  // One 4-byte index per manifest family, allocated once and reused for every group.
  std::vector<int> filteredIndices_;
  freeink::ui::ListNav groupNav_;

  // Download progress
  size_t currentFileIndex_ = 0;
  size_t currentFileTotal_ = 0;
  size_t fileProgress_ = 0;
  size_t fileTotal_ = 0;
  int downloadingFamilyIndex_ = 0;
  std::string errorMessage_;
  bool cancelRequested_ = false;
  // Set when the cancel came from the home gesture (consumed by the download
  // callback's own input pump); exit to home after the abort unwinds.
  bool goHomeRequested_ = false;

  // Shared cache for group and family rows. It is rebuilt only when the visible
  // list changes, never for cursor movement or tap flash repaints.
  std::vector<std::string> rowLabels_;
  std::vector<freeink::ui::ListItem> rowItems_;
  bool rowsDirty_ = true;
  void rebuildRowItems();
  void rebuildGroupRowItems();
  void rebuildFamilyRowItems();

  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  freeink::ui::ListNav& activeNav() override;
  void onBackButton() override;
  // Non-list states (loading, downloading, complete, error) consume the loop
  // pass here; the group and family lists use the base list protocol.
  bool handleCustomInput() override;

  void activateSelected();

  void onWifiSelectionComplete(bool success);
  bool fetchAndParseManifest();
  void downloadFamily(ManifestFamily& family);
  void downloadAll();
  void updateAll();
  static bool computeFileCrc32(const char* path, uint32_t& outCrc);
  bool showDownloadAllRow() const;
  bool showUpdateAllRow() const;
  int specialRowCount() const;
  bool isDownloadAllRow(int index) const;
  bool isUpdateAllRow(int index) const;
  bool isSelectedFamilyDeletable() const;
  void promptDeleteSelectedFamily();
  void onDeleteConfirmationResult(const ActivityResult& result);
  int familyIndexFromList(int listIndex) const;
  int listItemCount() const;
  bool hasGroupScreen() const { return !scriptGroupLabels_.empty(); }
  int groupListItemCount() const { return 1 + static_cast<int>(scriptGroupLabels_.size()); }
  int groupMemberCount(int scriptGroupIndex) const;
  void buildFilteredIndices(int groupListIndex);
  void enterGroup(int groupListIndex);
  size_t totalDownloadSize() const;
  size_t totalUpdateSize() const;
  static std::string formatSize(size_t bytes);
};
