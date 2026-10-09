#pragma once

#include "activities/CatalogActivity.h"

class PluginHubInstallActivity final : public CatalogActivity {
 public:
  explicit PluginHubInstallActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                    bool showConfirmation = true);

  static bool isAvailable();
  void onEnter() override;

 private:
  bool showConfirmation = true;

  int listCount() const override { return 0; }
  bool hasSearch() const override { return false; }
  void activateIndex(int) override {}
  void buildScreen(UiScreen& screen) override;
  bool handleCustomInput() override;
  void onBackButton() override;
  void startBrowse() override;
  void retryBrowse() override { startBrowse(); }
  void performSearch(const std::string&) override {}
  void downloadFinished(bool cancelled) override;
  void wifiSelectionCancelled() override;

  HttpDownloader::DownloadError installPluginHub();
  static void cleanupStagedFiles();
};
