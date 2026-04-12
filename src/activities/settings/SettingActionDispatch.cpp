#include "SettingActionDispatch.h"

#include "ButtonRemapActivity.h"
#include "CalibreSettingsActivity.h"
#include "ClearCacheActivity.h"
#include "ClockSettingsActivity.h"
#include "DetectTimezoneActivity.h"
#include "KOReaderSettingsActivity.h"
#include "LanguageSelectActivity.h"
#include "OtaUpdateActivity.h"
#include "StatusBarSettingsActivity.h"
#include "SyncTimeActivity.h"
#include "SystemInformationActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/weather/WeatherSettingsActivity.h"

std::unique_ptr<Activity> createActivityForAction(SettingAction action, GfxRenderer& renderer,
                                                  MappedInputManager& mappedInput) {
  switch (action) {
    case SettingAction::RemapFrontButtons:
      return std::make_unique<ButtonRemapActivity>(renderer, mappedInput);
    case SettingAction::CustomiseStatusBar:
      return std::make_unique<StatusBarSettingsActivity>(renderer, mappedInput);
    case SettingAction::ClockSettings:
      return std::make_unique<ClockSettingsActivity>(renderer, mappedInput);
    case SettingAction::KOReaderSync:
      return std::make_unique<KOReaderSettingsActivity>(renderer, mappedInput);
    case SettingAction::OPDSBrowser:
      return std::make_unique<CalibreSettingsActivity>(renderer, mappedInput);
    case SettingAction::Network:
      return std::make_unique<WifiSelectionActivity>(renderer, mappedInput, false);
    case SettingAction::ClearCache:
      return std::make_unique<ClearCacheActivity>(renderer, mappedInput);
    case SettingAction::CheckForUpdates:
      return std::make_unique<OtaUpdateActivity>(renderer, mappedInput);
    case SettingAction::Language:
      return std::make_unique<LanguageSelectActivity>(renderer, mappedInput);
    case SettingAction::Weather:
      return std::make_unique<WeatherSettingsActivity>(renderer, mappedInput);
    case SettingAction::SystemInfo:
      return std::make_unique<SystemInformationActivity>(renderer, mappedInput);
    case SettingAction::SyncTime:
      return std::make_unique<SyncTimeActivity>(renderer, mappedInput);
    case SettingAction::DetectTimezone:
      return std::make_unique<DetectTimezoneActivity>(renderer, mappedInput);
    case SettingAction::Submenu:
    case SettingAction::None:
      return nullptr;
  }
  return nullptr;
}
