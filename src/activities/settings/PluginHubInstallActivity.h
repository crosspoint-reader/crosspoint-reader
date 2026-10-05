#pragma once

#include "activities/CatalogActivity.h"

class PluginHubInstallActivity final : public CatalogActivity {
 public:
  explicit PluginHubInstallActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  static bool isInstalled();
  void onEnter() override;

 private:
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

  HttpDownloader::DownloadError installPluginHub();
  static void cleanupStagedFiles();
};
