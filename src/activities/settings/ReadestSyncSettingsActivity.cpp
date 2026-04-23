#include "ReadestSyncSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <cctype>

#include "MappedInputManager.h"
#include "ReadestAuthActivity.h"
#include "ReadestCredentialStore.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int MENU_ITEMS = 4;
const StrId menuNames[MENU_ITEMS] = {StrId::STR_USERNAME, StrId::STR_PASSWORD, StrId::STR_SYNC_SERVER_URL,
                                     StrId::STR_AUTHENTICATE};
}  // namespace

void ReadestSyncSettingsActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  showCredentialsError = false;
  requestUpdate();
}

void ReadestSyncSettingsActivity::onExit() { Activity::onExit(); }

void ReadestSyncSettingsActivity::loop() {
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

void ReadestSyncSettingsActivity::handleSelection() {
  if (selectedIndex == 0) {
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_READEST_USERNAME),
                                                                   READEST_STORE.getUsername(), 64, InputType::Text),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               READEST_STORE.setCredentials(kb.text, READEST_STORE.getPassword());
                               if (!READEST_STORE.saveToFile())
                                 LOG_ERR("RSS", "Storage.writeFile() failed saving username");
                             }
                           });
  } else if (selectedIndex == 1) {
    startActivityForResult(
        std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_READEST_PASSWORD),
                                                READEST_STORE.getPassword(), 64, InputType::Password),
        [this](const ActivityResult& result) {
          if (!result.isCancelled) {
            const auto& kb = std::get<KeyboardResult>(result.data);
            READEST_STORE.setCredentials(READEST_STORE.getUsername(), kb.text);
            if (!READEST_STORE.saveToFile()) LOG_ERR("RSS", "Storage.writeFile() failed saving password");
          }
        });
  } else if (selectedIndex == 2) {
    const std::string currentUrl = READEST_STORE.getServerUrl();
    const std::string prefillUrl = currentUrl.empty() ? "https://" : currentUrl;
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SYNC_SERVER_URL),
                                                                   prefillUrl, 128, InputType::Url),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               std::string trimmed = kb.text;
                               while (!trimmed.empty() && isspace(static_cast<unsigned char>(trimmed.back())))
                                 trimmed.pop_back();
                               size_t start = 0;
                               while (start < trimmed.size() && isspace(static_cast<unsigned char>(trimmed[start])))
                                 ++start;
                               trimmed.erase(0, start);
                               std::string lower = trimmed;
                               for (auto& c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
                               const std::string urlToSave = (lower == "http://" || lower == "https://") ? "" : trimmed;
                               READEST_STORE.setServerUrl(urlToSave);
                               if (!READEST_STORE.saveToFile())
                                 LOG_ERR("RSS", "Storage.writeFile() failed saving server URL");
                             }
                           });
  } else if (selectedIndex == 3) {
    if (!READEST_STORE.hasCredentials() || READEST_STORE.getServerUrl().empty()) {
      showCredentialsError = true;
      requestUpdate();
      return;
    }
    startActivityForResult(std::make_unique<ReadestAuthActivity>(renderer, mappedInput), [](const ActivityResult&) {});
  }
}

void ReadestSyncSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_READEST_SYNC));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(MENU_ITEMS),
      static_cast<int>(selectedIndex), [](int index) { return std::string(I18N.get(menuNames[index])); }, nullptr,
      nullptr,
      [this](int index) {
        if (index == 0) {
          auto username = READEST_STORE.getUsername();
          return username.empty() ? std::string(tr(STR_NOT_SET)) : username;
        } else if (index == 1) {
          return READEST_STORE.getPassword().empty() ? std::string(tr(STR_NOT_SET)) : std::string("******");
        } else if (index == 2) {
          auto serverUrl = READEST_STORE.getServerUrl();
          return serverUrl.empty() ? std::string(tr(STR_NOT_SET)) : serverUrl;
        } else if (index == 3) {
          const bool canAuth = READEST_STORE.hasCredentials() && !READEST_STORE.getServerUrl().empty();
          return canAuth ? "" : std::string("[") + tr(STR_SET_CREDENTIALS_FIRST) + "]";
        }
        return std::string(tr(STR_NOT_SET));
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (showCredentialsError) {
    showCredentialsError = false;
    GUI.drawPopup(renderer, tr(STR_SET_CREDENTIALS_FIRST));
  }

  renderer.displayBuffer();
}
