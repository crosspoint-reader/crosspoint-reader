#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

#ifndef X4_EBOOKS_LIST_URL
#define X4_EBOOKS_LIST_URL "https://n8n.beckmann-md.de/webhook/x4-ebooks-list"
#endif

#ifndef X4_EBOOKS_DOWNLOAD_URL
#define X4_EBOOKS_DOWNLOAD_URL "https://n8n.beckmann-md.de/webhook/x4-ebooks-download"
#endif

class EbookSyncActivity : public Activity {
 public:
  explicit EbookSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state_ == LOADING_LIST || state_ == SYNCING || state_ == COMPLETE || state_ == ERROR; }
  bool skipLoopDelay() override { return true; }

 private:
  enum State {
    WIFI_SELECTION,
    LOADING_LIST,
    READY,
    SYNCING,
    COMPLETE,
    ERROR,
  };

  struct EbookEntry {
    std::string path;
    std::string filename;
    std::string localPath;
    bool exists = false;
  };

  State state_ = WIFI_SELECTION;
  ButtonNavigator buttonNavigator_;
  std::vector<EbookEntry> entries_;
  int selectedIndex_ = 0;
  size_t currentIndex_ = 0;
  size_t newDownloads_ = 0;
  size_t skippedExisting_ = 0;
  size_t fileProgress_ = 0;
  size_t fileTotal_ = 0;
  std::string statusMessage_;
  std::string errorMessage_;
  bool cancelRequested_ = false;

  void onWifiSelectionComplete(bool success);
  bool fetchAndParseList();
  void syncAllNew();
  bool downloadEntry(EbookEntry& entry);
  int listItemCount() const;
  static std::string urlEncode(const std::string& value);
  static std::string ensureExtensionPreserved(const std::string& filename, const std::string& originalPath);
};
