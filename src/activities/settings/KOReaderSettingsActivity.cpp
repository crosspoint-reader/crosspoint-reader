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
        renderer, mappedInput, tr(STR_KOREADER_USERNAME), KOREADER_STORE.getUsername(), 10,
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
        renderer, mappedInput, tr(STR_KOREADER_PASSWORD), KOREADER_STORE.getPassword(), 10,
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
        renderer, mappedInput, tr(STR_SYNC_SERVER_URL), prefillUrl, 10,
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

  const auto pageWidth = renderer.getScreenWidth();

  // Draw header
  renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_KOREADER_SYNC), true, EpdFontFamily::BOLD);

  // Draw selection highlight
  renderer.fillRect(0, 60 + selectedIndex * 30 - 2, pageWidth - 1, 30);

  // Draw menu items
  for (int i = 0; i < MENU_ITEMS; i++) {
    const int settingY = 60 + i * 30;
    const bool isSelected = (i == selectedIndex);

    renderer.drawText(UI_10_FONT_ID, 20, settingY, I18N.get(menuNames[i]), !isSelected);

    // Draw status for each item
    std::string status = "";
    if (i == 0) {
      status = std::string("[") + (KOREADER_STORE.getUsername().empty() ? tr(STR_NOT_SET) : tr(STR_SET)) + "]";
    } else if (i == 1) {
      status = std::string("[") + (KOREADER_STORE.getPassword().empty() ? tr(STR_NOT_SET) : tr(STR_SET)) + "]";
    } else if (i == 2) {
      status =
          std::string("[") + (KOREADER_STORE.getServerUrl().empty() ? tr(STR_DEFAULT_VALUE) : tr(STR_CUSTOM)) + "]";
    } else if (i == 3) {
      status = std::string("[") +
               (KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME ? tr(STR_FILENAME) : tr(STR_BINARY)) +
               "]";
    } else if (i == 4) {
      status = KOREADER_STORE.hasCredentials() ? "" : std::string("[") + tr(STR_SET_CREDENTIALS_FIRST) + "]";
    }

    const auto width = renderer.getTextWidth(UI_10_FONT_ID, status.c_str());
    renderer.drawText(UI_10_FONT_ID, pageWidth - 20 - width, settingY, status.c_str(), !isSelected);
  }

  // Draw button hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
