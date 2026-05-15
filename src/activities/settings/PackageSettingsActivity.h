#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "marginalia/PackageStore.h"
#include "util/ButtonNavigator.h"

class PackageSettingsActivity final : public Activity {
 public:
  PackageSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Marginalia::PackageManifest package);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  Marginalia::PackageManifest package_;
  ButtonNavigator buttonNavigator_;
  int selectedIndex_ = 0;

  std::string settingValueLabel(const Marginalia::PackageSettingDefinition& setting) const;
  void handleSelection();
};
