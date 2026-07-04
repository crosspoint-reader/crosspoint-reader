#pragma once

#include <AppCatalogEntry.h>
#include <AppRegistryJson.h>
#include <AppStoreManifestMeta.h>

#include <unordered_map>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class DiscoverAppsActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
  std::vector<AppCatalogEntry> entries_;
  std::unordered_map<std::string, AppRegistryEntry> installedById_;
  AppStoreManifestMetaData manifestMeta_;
  bool hasManifestMeta_ = false;
  bool hasAttemptedLoad_ = false;

  void loadCatalog();
  void onWifiSelectionComplete(bool success);
  bool installSelectedEntry();
  [[nodiscard]] const AppRegistryEntry* findInstalledEntry(const std::string& appId) const;
  void sortEntries();
  [[nodiscard]] std::string formatInstalledSubtitle(const std::string& dateDisplay) const;
  [[nodiscard]] std::string formatAvailableCountLabel() const;
  [[nodiscard]] std::string formatInstalledCountLabel() const;

 public:
  explicit DiscoverAppsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
