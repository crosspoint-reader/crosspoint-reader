#include "ApplicationsMenuActivity.h"

#include <AppDateTimeFormat.h>
#include <AppListLabels.h>
#include <AppRegistry.h>
#include <AppStoreManifest.h>
#include <AppStoreManifestMeta.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "AppRunnerActivity.h"
#include "CrossPointSettings.h"
#include "DiscoverAppsActivity.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

ApplicationsMenuActivity::ApplicationsMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Applications", renderer, mappedInput) {}

void ApplicationsMenuActivity::reloadEntries() {
  APP_REGISTRY.loadFromFile();
  entries_ = APP_REGISTRY.getEntries();

  APP_STORE_CATALOG.loadFromCache();
  catalogById_.clear();
  for (const auto& entry : APP_STORE_CATALOG.getEntries()) {
    catalogById_[entry.id] = entry;
  }

  hasManifestMeta_ = AppStoreManifestMeta::readMeta(manifestMeta_);

  std::sort(entries_.begin(), entries_.end(), [](const AppRegistryEntry& a, const AppRegistryEntry& b) {
    if (AppDateTimeFormat::isNewerInstalledAt(a.installedAt, b.installedAt)) {
      return true;
    }
    if (AppDateTimeFormat::isNewerInstalledAt(b.installedAt, a.installedAt)) {
      return false;
    }
    return a.name < b.name;
  });
}

std::string ApplicationsMenuActivity::formatInstalledSubtitle(const std::string& dateDisplay) const {
  if (dateDisplay.empty()) {
    return "";
  }
  char buf[64];
  snprintf(buf, sizeof(buf), tr(STR_APPS_INSTALLED_ON), dateDisplay.c_str());
  return buf;
}

std::string ApplicationsMenuActivity::formatInstalledCountLabel() const {
  char buf[32];
  snprintf(buf, sizeof(buf), tr(STR_APPS_INSTALLED_COUNT), static_cast<int>(entries_.size()));
  return buf;
}

std::string ApplicationsMenuActivity::formatBrowseCountLabel() const {
  if (!hasManifestMeta_) {
    return "";
  }
  char buf[32];
  snprintf(buf, sizeof(buf), tr(STR_APPS_BROWSE_COUNT), static_cast<int>(manifestMeta_.appCount));
  return buf;
}

int ApplicationsMenuActivity::totalItemCount() const {
  return kMenuItemCount + static_cast<int>(entries_.size());
}

bool ApplicationsMenuActivity::isMenuIndex(const int index) { return index < kMenuItemCount; }

int ApplicationsMenuActivity::appIndexFor(const int index) const { return index - kMenuItemCount; }

void ApplicationsMenuActivity::onEnter() {
  Activity::onEnter();
  reloadEntries();
  selectedIndex = 0;
  requestUpdate();
}

void ApplicationsMenuActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onGoHome(HomeMenuItem::APPLICATIONS);
    return;
  }

  const int itemCount = totalItemCount();

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (isMenuIndex(selectedIndex)) {
      activityManager.replaceActivity(std::make_unique<DiscoverAppsActivity>(renderer, mappedInput));
      return;
    }

    const int appIndex = appIndexFor(selectedIndex);
    if (appIndex < 0 || appIndex >= static_cast<int>(entries_.size())) {
      return;
    }
    const std::string appId = entries_[static_cast<size_t>(appIndex)].id;
    const std::string& appName = entries_[static_cast<size_t>(appIndex)].name;
    startActivityForResult(std::make_unique<AppRunnerActivity>(renderer, mappedInput, appId, appName),
                           [this](const ActivityResult&) {
                             reloadEntries();
                             const int count = totalItemCount();
                             if (selectedIndex >= count) {
                               selectedIndex = count - 1;
                             }
                             if (selectedIndex < 0) {
                               selectedIndex = 0;
                             }
                             requestUpdate();
                           });
    return;
  }

  buttonNavigator.onNextRelease([this, itemCount] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, itemCount);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, itemCount] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, itemCount);
    requestUpdate();
  });
}

void ApplicationsMenuActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const bool use12Hour = SETTINGS.clockFormat == 1;
  const uint8_t utcOffset = SETTINGS.clockUtcOffsetQ;

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_APPLICATIONS));
  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight}, "",
                    formatInstalledCountLabel().c_str());

  const int contentTop =
      metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
  const int contentBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;

  const bool discoverHasSubtitle = hasManifestMeta_;
  const int menuRowHeight =
      discoverHasSubtitle ? metrics.listWithSubtitleRowHeight : metrics.listRowHeight;
  const int menuHeight = kMenuItemCount * menuRowHeight;
  const int separatorY = contentTop + menuHeight + metrics.verticalSpacing / 2;
  const int appsTop = contentTop + menuHeight + metrics.verticalSpacing;
  const int appsHeight = contentBottom - appsTop;

  const int menuSelected = isMenuIndex(selectedIndex) ? selectedIndex : -1;
  GUI.drawList(renderer, Rect{0, contentTop, pageWidth, menuHeight}, kMenuItemCount, menuSelected,
               [](int index) {
                 (void)index;
                 return std::string(tr(STR_DISCOVER_APPS));
               },
               [this](int index) {
                 (void)index;
                 return formatBrowseCountLabel();
               });

  renderer.drawLine(0, separatorY, pageWidth - 1, separatorY, 3, true);

  const int appCount = static_cast<int>(entries_.size());
  if (appCount == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, appsTop + appsHeight / 2, tr(STR_NO_INSTALLED_APPS));
  } else {
    const int appSelected = isMenuIndex(selectedIndex) ? -1 : appIndexFor(selectedIndex);
    GUI.drawList(
        renderer, Rect{0, appsTop, pageWidth, appsHeight}, appCount, appSelected,
        [this](int index) { return entries_[static_cast<size_t>(index)].name; },
        [this, utcOffset, use12Hour](int index) {
          const auto& entry = entries_[static_cast<size_t>(index)];
          const auto* catalog = [&]() -> const AppCatalogEntry* {
            const auto it = catalogById_.find(entry.id);
            return it != catalogById_.end() ? &it->second : nullptr;
          }();
          const auto labels = AppListLabels::buildInstalledRowLabels(entry, catalog, utcOffset, use12Hour);
          return formatInstalledSubtitle(labels.subtitle);
        },
        nullptr,
        [this, utcOffset, use12Hour](int index) {
          const auto& entry = entries_[static_cast<size_t>(index)];
          const auto* catalog = [&]() -> const AppCatalogEntry* {
            const auto it = catalogById_.find(entry.id);
            return it != catalogById_.end() ? &it->second : nullptr;
          }();
          return AppListLabels::buildInstalledRowLabels(entry, catalog, utcOffset, use12Hour).rowValue;
        },
        true);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
