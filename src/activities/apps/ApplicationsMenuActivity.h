#pragma once

#include <AppCatalogEntry.h>
#include <AppRegistryJson.h>
#include <AppStoreManifestMeta.h>

#include <unordered_map>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class ApplicationsMenuActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
  std::vector<AppRegistryEntry> entries_;
  std::unordered_map<std::string, AppCatalogEntry> catalogById_;
  AppStoreManifestMetaData manifestMeta_;
  bool hasManifestMeta_ = false;

  static constexpr int kMenuItemCount = 1;

  void reloadEntries();
  [[nodiscard]] int totalItemCount() const;
  [[nodiscard]] static bool isMenuIndex(int index);
  [[nodiscard]] int appIndexFor(int index) const;
  [[nodiscard]] std::string formatInstalledSubtitle(const std::string& dateDisplay) const;
  [[nodiscard]] std::string formatInstalledCountLabel() const;
  [[nodiscard]] std::string formatBrowseCountLabel() const;

 public:
  explicit ApplicationsMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
