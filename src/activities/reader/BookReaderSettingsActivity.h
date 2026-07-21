#pragma once

#include <vector>

#include "PerBookReaderSettings.h"
#include "activities/Activity.h"
#include "activities/settings/SettingsActivity.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"

class BookReaderSettingsActivity final : public Activity {
 public:
  BookReaderSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                             PerBookReaderSettings globalDefaults, PerBookReaderSettings bookSettings);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  OptionPopup optionPopup;
  std::vector<SettingInfo> settings;
  PerBookReaderSettings globalDefaults;
  PerBookReaderSettings savedCustom;
  bool customEnabled = false;
  bool resetConfirmationPending = false;
  int selectedIndex = 0;

  void rebuildSettings();
  void toggleSelected();
  void setCustomEnabled(bool enabled);
  void finishWithResult();
  std::string valueForRow(int index) const;
};
