#include "KoofrSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "KoofrCredentialStore.h"
#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int MENU_ITEMS = 4;
const StrId menuNames[MENU_ITEMS] = {StrId::STR_KOOFR_EMAIL, StrId::STR_KOOFR_APP_PASSWORD,
                                     StrId::STR_KOOFR_REMOTE_FOLDER, StrId::STR_KOOFR_WEBDAV_URL};
}  // namespace

void KoofrSettingsActivity::onEnter() {
  Activity::onEnter();

  selectedIndex = 0;
  requestUpdate();
}

void KoofrSettingsActivity::onExit() { Activity::onExit(); }

void KoofrSettingsActivity::loop() {
  auto activateSelected = [this] { handleSelection(); };

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    activateSelected();
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  int touchSel = static_cast<int>(selectedIndex);
  const auto listTouch = handleListTouch(touchSel, MENU_ITEMS, contentTop, contentHeight, false);
  if (listTouch != ListTouchResult::None) {
    selectedIndex = static_cast<size_t>(touchSel);
    if (listTouch == ListTouchResult::Activated) activateSelected();
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

void KoofrSettingsActivity::handleSelection() {
  if (selectedIndex == 0) {
    // Account e-mail
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_KOOFR_EMAIL),
                                                                   KOOFR_STORE.getUsername(), 64, InputType::Text),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               KOOFR_STORE.setCredentials(kb.text, KOOFR_STORE.getPassword());
                               KOOFR_STORE.saveToFile();
                             }
                           });
  } else if (selectedIndex == 1) {
    // App password — Koofr's WebDAV endpoint rejects the login password
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_KOOFR_APP_PASSWORD),
                                                                   KOOFR_STORE.getPassword(), 64, InputType::Password),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               KOOFR_STORE.setCredentials(KOOFR_STORE.getUsername(), kb.text);
                               KOOFR_STORE.saveToFile();
                             }
                           });
  } else if (selectedIndex == 2) {
    // Destination folder, relative to the WebDAV root
    startActivityForResult(
        std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_KOOFR_REMOTE_FOLDER),
                                                KOOFR_STORE.getEffectiveRemoteDir(), 128, InputType::Text),
        [this](const ActivityResult& result) {
          if (!result.isCancelled) {
            const auto& kb = std::get<KeyboardResult>(result.data);
            KOOFR_STORE.setRemoteDir(kb.text);
            KOOFR_STORE.saveToFile();
          }
        });
  } else if (selectedIndex == 3) {
    // WebDAV URL — prefill with https:// if empty to save typing
    const std::string currentUrl = KOOFR_STORE.getServerUrl();
    const std::string prefillUrl = currentUrl.empty() ? "https://" : currentUrl;
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_KOOFR_WEBDAV_URL),
                                                                   prefillUrl, 128, InputType::Url),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               const std::string urlToSave =
                                   (kb.text == "https://" || kb.text == "http://") ? "" : kb.text;
                               KOOFR_STORE.setServerUrl(urlToSave);
                               KOOFR_STORE.saveToFile();
                             }
                           });
  }
}

void KoofrSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_KOOFR_SYNC));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, MENU_ITEMS, static_cast<int>(selectedIndex),
      [](int index) { return std::string(I18N.get(menuNames[index])); }, nullptr, nullptr,
      [](int index) {
        if (index == 0) {
          const auto& username = KOOFR_STORE.getUsername();
          return username.empty() ? std::string(tr(STR_NOT_SET)) : username;
        }
        if (index == 1) {
          return KOOFR_STORE.getPassword().empty() ? std::string(tr(STR_NOT_SET)) : std::string("******");
        }
        if (index == 2) {
          return KOOFR_STORE.getEffectiveRemoteDir();
        }
        // WebDAV URL: show the default that is actually in use, scheme stripped
        // for space, when the user hasn't overridden it.
        const auto& serverUrl = KOOFR_STORE.getServerUrl();
        if (!serverUrl.empty()) return serverUrl;
        std::string defaultUrl = KOOFR_STORE.getBaseUrl();
        const auto schemeEnd = defaultUrl.find("://");
        if (schemeEnd != std::string::npos) {
          defaultUrl.erase(0, schemeEnd + 3);
        }
        return std::string(tr(STR_DEFAULT_VALUE)) + ": " + defaultUrl;
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
