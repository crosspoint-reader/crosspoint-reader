#include "KOReaderSettingsActivity.h"

#include <GfxRenderer.h>

#include <cstring>

#include "KOReaderAuthActivity.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "fontIds.h"

namespace {
constexpr int MENU_ITEMS = 4;
const char* menuNames[MENU_ITEMS] = {"Username", "Password", "Sync Server URL", "Authenticate"};
}  // namespace

void KOReaderSettingsActivity::taskTrampoline(void* param) {
  auto* self = static_cast<KOReaderSettingsActivity*>(param);
  self->displayTaskLoop();
}

void KOReaderSettingsActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();
  selectedIndex = 0;
  updateRequired = true;

  xTaskCreate(&KOReaderSettingsActivity::taskTrampoline, "KOReaderSettingsTask",
              4096,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void KOReaderSettingsActivity::onExit() {
  ActivityWithSubactivity::onExit();

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

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

  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    selectedIndex = (selectedIndex + MENU_ITEMS - 1) % MENU_ITEMS;
    updateRequired = true;
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
             mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    selectedIndex = (selectedIndex + 1) % MENU_ITEMS;
    updateRequired = true;
  }
}

void KOReaderSettingsActivity::handleSelection() {
  xSemaphoreTake(renderingMutex, portMAX_DELAY);

  if (selectedIndex == 0) {
    // Username
    exitActivity();
    enterNewActivity(new KeyboardEntryActivity(
        renderer, mappedInput, "KOReader Username", KOREADER_STORE.getUsername(), 10,
        64,     // maxLength
        false,  // not password
        [this](const std::string& username) {
          KOREADER_STORE.setCredentials(username, KOREADER_STORE.getPassword());
          KOREADER_STORE.saveToFile();
          exitActivity();
          updateRequired = true;
        },
        [this]() {
          exitActivity();
          updateRequired = true;
        }));
  } else if (selectedIndex == 1) {
    // Password
    exitActivity();
    enterNewActivity(new KeyboardEntryActivity(
        renderer, mappedInput, "KOReader Password", KOREADER_STORE.getPassword(), 10,
        64,     // maxLength
        false,  // show characters
        [this](const std::string& password) {
          KOREADER_STORE.setCredentials(KOREADER_STORE.getUsername(), password);
          KOREADER_STORE.saveToFile();
          exitActivity();
          updateRequired = true;
        },
        [this]() {
          exitActivity();
          updateRequired = true;
        }));
  } else if (selectedIndex == 2) {
    // Sync Server URL
    exitActivity();
    enterNewActivity(new KeyboardEntryActivity(
        renderer, mappedInput, "Sync Server URL", KOREADER_STORE.getServerUrl(), 10,
        128,    // maxLength - URLs can be long
        false,  // not password
        [this](const std::string& url) {
          KOREADER_STORE.setServerUrl(url);
          KOREADER_STORE.saveToFile();
          exitActivity();
          updateRequired = true;
        },
        [this]() {
          exitActivity();
          updateRequired = true;
        }));
  } else if (selectedIndex == 3) {
    // Authenticate
    if (!KOREADER_STORE.hasCredentials()) {
      // Can't authenticate without credentials - just show message briefly
      xSemaphoreGive(renderingMutex);
      return;
    }
    exitActivity();
    enterNewActivity(new KOReaderAuthActivity(renderer, mappedInput, [this] {
      exitActivity();
      updateRequired = true;
    }));
  }

  xSemaphoreGive(renderingMutex);
}

void KOReaderSettingsActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired && !subActivity) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void KOReaderSettingsActivity::render() {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();

  // Draw header
  renderer.drawCenteredText(UI_12_FONT_ID, 15, "KOReader Sync", true, EpdFontFamily::BOLD);

  // Draw selection highlight
  renderer.fillRect(0, 60 + selectedIndex * 30 - 2, pageWidth - 1, 30);

  // Draw menu items
  for (int i = 0; i < MENU_ITEMS; i++) {
    const int settingY = 60 + i * 30;
    const bool isSelected = (i == selectedIndex);

    renderer.drawText(UI_10_FONT_ID, 20, settingY, menuNames[i], !isSelected);

    // Draw status for each item
    const char* status = "";
    if (i == 0) {
      status = KOREADER_STORE.getUsername().empty() ? "[Not Set]" : "[Set]";
    } else if (i == 1) {
      status = KOREADER_STORE.getPassword().empty() ? "[Not Set]" : "[Set]";
    } else if (i == 2) {
      status = KOREADER_STORE.getServerUrl().empty() ? "[Not Set]" : "[Set]";
    } else if (i == 3) {
      status = KOREADER_STORE.hasCredentials() ? "" : "[Set credentials first]";
    }

    const auto width = renderer.getTextWidth(UI_10_FONT_ID, status);
    renderer.drawText(UI_10_FONT_ID, pageWidth - 20 - width, settingY, status, !isSelected);
  }

  // Draw button hints
  const auto labels = mappedInput.mapLabels("« Back", "Select", "", "");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
