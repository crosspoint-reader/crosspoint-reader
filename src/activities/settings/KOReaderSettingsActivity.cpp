#include "KOReaderSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstring>

#include "KOReaderAuthActivity.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int MENU_ITEMS = 5;
const StrId menuNames[MENU_ITEMS] = {StrId::STR_USERNAME, StrId::STR_PASSWORD, StrId::STR_SYNC_SERVER_URL,
                                     StrId::STR_DOCUMENT_MATCHING, StrId::STR_AUTHENTICATE};
}  // namespace

void KOReaderSettingsActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  selectedIndex = 0;
  requestUpdate();
}

void KOReaderSettingsActivity::onExit() { ActivityWithSubactivity::onExit(); }

void KOReaderSettingsActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onBack();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  // Handle navigation
  buttonNavigator.onNext([this] {
    selectedIndex = (selectedIndex + 1) % MENU_ITEMS;
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = (selectedIndex + MENU_ITEMS - 1) % MENU_ITEMS;
    requestUpdate();
  });
}

void KOReaderSettingsActivity::handleSelection() {
  if (selectedIndex == 0) {
    // Username
    exitActivity();
    enterNewActivity(new KeyboardEntryActivity(
        renderer, mappedInput, tr(STR_KOREADER_USERNAME), KOREADER_STORE.getUsername(),
        64,     // maxLength
        false,  // not password
        [this](const std::string& username) {
          KOREADER_STORE.setCredentials(username, KOREADER_STORE.getPassword());
          KOREADER_STORE.saveToFile();
          exitActivity();
          requestUpdate();
        },
        [this]() {
          exitActivity();
          requestUpdate();
        }));
  } else if (selectedIndex == 1) {
    // Password
    exitActivity();
    enterNewActivity(new KeyboardEntryActivity(
        renderer, mappedInput, tr(STR_KOREADER_PASSWORD), KOREADER_STORE.getPassword(),
        64,     // maxLength
        false,  // show characters
        [this](const std::string& password) {
          KOREADER_STORE.setCredentials(KOREADER_STORE.getUsername(), password);
          KOREADER_STORE.saveToFile();
          exitActivity();
          requestUpdate();
        },
        [this]() {
          exitActivity();
          requestUpdate();
        }));
  } else if (selectedIndex == 2) {
    // Sync Server URL - prefill with https:// if empty to save typing
    const std::string currentUrl = KOREADER_STORE.getServerUrl();
    const std::string prefillUrl = currentUrl.empty() ? "https://" : currentUrl;
    exitActivity();
    enterNewActivity(new KeyboardEntryActivity(
        renderer, mappedInput, tr(STR_SYNC_SERVER_URL), prefillUrl,
        128,    // maxLength - URLs can be long
        false,  // not password
        [this](const std::string& url) {
          // Clear if user just left the prefilled https://
          const std::string urlToSave = (url == "https://" || url == "http://") ? "" : url;
          KOREADER_STORE.setServerUrl(urlToSave);
          KOREADER_STORE.saveToFile();
          exitActivity();
          requestUpdate();
        },
        [this]() {
          exitActivity();
          requestUpdate();
        }));
  } else if (selectedIndex == 3) {
    // Document Matching - toggle between Filename and Binary
    const auto current = KOREADER_STORE.getMatchMethod();
    const auto newMethod =
        (current == DocumentMatchMethod::FILENAME) ? DocumentMatchMethod::BINARY : DocumentMatchMethod::FILENAME;
    KOREADER_STORE.setMatchMethod(newMethod);
    KOREADER_STORE.saveToFile();
    requestUpdate();
  } else if (selectedIndex == 4) {
    // Authenticate
    if (!KOREADER_STORE.hasCredentials()) {
      // Can't authenticate without credentials - just show message briefly
      return;
    }
    exitActivity();
    enterNewActivity(new KOReaderAuthActivity(renderer, mappedInput, [this] {
      exitActivity();
      requestUpdate();
    }));
  }
}

void KOReaderSettingsActivity::render(Activity::RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_KOREADER_SYNC));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(MENU_ITEMS),
      static_cast<int>(selectedIndex), [](int index) { return std::string(I18N.get(menuNames[index])); }, nullptr,
      nullptr,
      [this](int index) {
        // Draw status for each setting
        if (index == 0) {
          auto username = KOREADER_STORE.getUsername();
          return username.empty() ? std::string(tr(STR_NOT_SET)) : username;
        } else if (index == 1) {
          return KOREADER_STORE.getPassword().empty() ? std::string(tr(STR_NOT_SET)) : std::string("******");
        } else if (index == 2) {
          auto serverUrl = KOREADER_STORE.getServerUrl();
          return serverUrl.empty() ? std::string(tr(STR_DEFAULT_VALUE)) : serverUrl;
        } else if (index == 3) {
          return KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME ? std::string(tr(STR_FILENAME))
                                                                                  : std::string(tr(STR_BINARY));
        } else if (index == 4) {
          return KOREADER_STORE.hasCredentials() ? "" : std::string("[") + tr(STR_SET_CREDENTIALS_FIRST) + "]";
        }
        return std::string(tr(STR_NOT_SET));
      },
      true);

  // Draw help text at bottom
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
