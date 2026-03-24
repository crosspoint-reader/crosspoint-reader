#include "WebDavSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int MENU_ITEMS = 3;
const StrId menuNames[MENU_ITEMS] = {StrId::STR_WEBDAV_SERVER_URL, StrId::STR_USERNAME, StrId::STR_PASSWORD};
}  // namespace

void WebDavSettingsActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  requestUpdate();
}

void WebDavSettingsActivity::onExit() { Activity::onExit(); }

void WebDavSettingsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  buttonNavigator.onNext([this] {
    selectedIndex = (selectedIndex + 1) % MENU_ITEMS;
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = (selectedIndex + MENU_ITEMS - 1) % MENU_ITEMS;
    requestUpdate();
  });
}

void WebDavSettingsActivity::handleSelection() {
  if (selectedIndex == 0) {
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_WEBDAV_SERVER_URL),
                                                                   SETTINGS.webdavServerUrl, 127, false),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               strncpy(SETTINGS.webdavServerUrl, kb.text.c_str(), sizeof(SETTINGS.webdavServerUrl) - 1);
                               SETTINGS.webdavServerUrl[sizeof(SETTINGS.webdavServerUrl) - 1] = '\0';
                               SETTINGS.saveToFile();
                             }
                           });
  } else if (selectedIndex == 1) {
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_USERNAME),
                                                                   SETTINGS.webdavUsername, 63, false),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               strncpy(SETTINGS.webdavUsername, kb.text.c_str(), sizeof(SETTINGS.webdavUsername) - 1);
                               SETTINGS.webdavUsername[sizeof(SETTINGS.webdavUsername) - 1] = '\0';
                               SETTINGS.saveToFile();
                             }
                           });
  } else if (selectedIndex == 2) {
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_PASSWORD),
                                                                   SETTINGS.webdavPassword, 63, false),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               strncpy(SETTINGS.webdavPassword, kb.text.c_str(), sizeof(SETTINGS.webdavPassword) - 1);
                               SETTINGS.webdavPassword[sizeof(SETTINGS.webdavPassword) - 1] = '\0';
                               SETTINGS.saveToFile();
                             }
                           });
  }
}

void WebDavSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_WEBDAV_BROWSER));
  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                    tr(STR_WEBDAV_URL_HINT));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + metrics.tabBarHeight;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(MENU_ITEMS),
      static_cast<int>(selectedIndex), [](int index) { return std::string(I18N.get(menuNames[index])); }, nullptr,
      nullptr,
      [this](int index) {
        if (index == 0) {
          return (strlen(SETTINGS.webdavServerUrl) > 0) ? std::string(SETTINGS.webdavServerUrl)
                                                        : std::string(tr(STR_NOT_SET));
        } else if (index == 1) {
          return (strlen(SETTINGS.webdavUsername) > 0) ? std::string(SETTINGS.webdavUsername)
                                                       : std::string(tr(STR_NOT_SET));
        } else if (index == 2) {
          return (strlen(SETTINGS.webdavPassword) > 0) ? std::string("******") : std::string(tr(STR_NOT_SET));
        }
        return std::string(tr(STR_NOT_SET));
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
