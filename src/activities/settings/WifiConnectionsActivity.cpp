#include "WifiConnectionsActivity.h"

#include <GfxRenderer.h>
#include <HardwareSerial.h>

#include "MappedInputManager.h"
#include "WifiCredentialStore.h"
#include "activities/network/WifiSelectionActivity.h"
#include "fontIds.h"

namespace {
constexpr int PAGE_ITEMS = 23;
constexpr int SKIP_PAGE_MS = 700;
constexpr unsigned long IGNORE_INPUT_MS = 300;
}

void WifiConnectionsActivity::taskTrampoline(void* param) {
  auto* self = static_cast<WifiConnectionsActivity*>(param);
  self->displayTaskLoop();
}

void WifiConnectionsActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();
  state = State::LIST;
  selectorIndex = 0;
  settingsSelection = 0;
  selectedNetwork.clear();
  enterTime = millis();
  updateRequired = true;

  WIFI_STORE.loadFromFile();

  xTaskCreate(&WifiConnectionsActivity::taskTrampoline, "WifiConnectionsTask",
              4096,
              this,
              1,
              &displayTaskHandle
  );
}

void WifiConnectionsActivity::onExit() {
  ActivityWithSubactivity::onExit();

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void WifiConnectionsActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  const unsigned long timeSinceEnter = millis() - enterTime;
  if (timeSinceEnter < IGNORE_INPUT_MS) {
    return;
  }

  if (state == State::SETTINGS_MENU) {
    // Check if this network is already the default
    WIFI_STORE.loadFromFile();
    const std::string currentDefault = WIFI_STORE.getDefaultSSID();
    const bool isDefault = (selectedNetwork == currentDefault);
    
    // Handle settings menu
    if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
        mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      if (settingsSelection > 0) {
        settingsSelection--;
        updateRequired = true;
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
               mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      if (settingsSelection < 1) {
        settingsSelection++;
        updateRequired = true;
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (settingsSelection == 0) {
        if (isDefault) {
          removeDefault();
        } else {
          setDefault();
        }
      } else {
        deleteNetwork();
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      cancelSettings();
    }
    return;
  }

  // Handle list navigation
  const auto& credentials = WIFI_STORE.getCredentials();
  const size_t totalItems = credentials.size() + 1;  // +1 for "Add new connection"

  const bool prevReleased = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool nextReleased = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Right);
  const bool skipPage = mappedInput.getHeldTime() > SKIP_PAGE_MS;

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectorIndex == 0) {
      // "Add new connection" selected - launch WiFi selection
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      exitActivity();
      enterNewActivity(new WifiSelectionActivity(renderer, mappedInput, [this](bool connected) {
        // Reload credentials after WiFi selection
        WIFI_STORE.loadFromFile();
        exitActivity();
        enterTime = millis();  // Reset enter time to ignore input after subactivity exits
        updateRequired = true;
      }, true));  // true = fromSettingsScreen (always save password and disconnect after)
      xSemaphoreGive(renderingMutex);
    } else {
      // Regular credential selected
      handleSettings();
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onBack();
  } else if (prevReleased) {
    if (skipPage) {
      selectorIndex = ((selectorIndex / PAGE_ITEMS - 1) * PAGE_ITEMS + totalItems) % totalItems;
    } else {
      selectorIndex = (selectorIndex + totalItems - 1) % totalItems;
    }
    updateRequired = true;
  } else if (nextReleased) {
    if (skipPage) {
      selectorIndex = ((selectorIndex / PAGE_ITEMS + 1) * PAGE_ITEMS) % totalItems;
    } else {
      selectorIndex = (selectorIndex + 1) % totalItems;
    }
    updateRequired = true;
  }
}

void WifiConnectionsActivity::handleSettings() {
  const auto& credentials = WIFI_STORE.getCredentials();
  // selectorIndex 0 is "Add new connection", so credentials start at index 1
  if (selectorIndex == 0 || selectorIndex > credentials.size()) {
    return;
  }

  selectedNetwork = credentials[selectorIndex - 1].ssid;
  state = State::SETTINGS_MENU;
  settingsSelection = 0;
  updateRequired = true;
}

void WifiConnectionsActivity::setDefault() {
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  WIFI_STORE.setDefaultSSID(selectedNetwork);
  xSemaphoreGive(renderingMutex);

  state = State::LIST;
  selectedNetwork.clear();
  updateRequired = true;
}

void WifiConnectionsActivity::removeDefault() {
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  WIFI_STORE.setDefaultSSID("");
  xSemaphoreGive(renderingMutex);

  state = State::LIST;
  selectedNetwork.clear();
  updateRequired = true;
}

void WifiConnectionsActivity::deleteNetwork() {
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  WIFI_STORE.removeCredential(selectedNetwork);
  xSemaphoreGive(renderingMutex);

  // Reload to get updated list
  WIFI_STORE.loadFromFile();

  const auto& credentials = WIFI_STORE.getCredentials();
  const size_t totalItems = credentials.size() + 1;
  if (selectorIndex > credentials.size()) {
    selectorIndex = credentials.size();
  }

  state = State::LIST;
  selectedNetwork.clear();
  updateRequired = true;
}

void WifiConnectionsActivity::cancelSettings() {
  state = State::LIST;
  selectedNetwork.clear();
  updateRequired = true;
}

void WifiConnectionsActivity::displayTaskLoop() {
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

void WifiConnectionsActivity::render() {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.drawCenteredText(UI_12_FONT_ID, 15, "WiFi Connections", true, EpdFontFamily::BOLD);

  if (state == State::SETTINGS_MENU) {
    const int centerY = pageHeight / 2;
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - 40, "Settings", true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - 20, selectedNetwork.c_str());

    // Check if this network is already the default
    const std::string currentDefault = WIFI_STORE.getDefaultSSID();
    const bool isDefault = (selectedNetwork == currentDefault);

    const char* defaultText = settingsSelection == 0 
      ? (isDefault ? "> Remove Default" : "> Set Default")
      : (isDefault ? "  Remove Default" : "  Set Default");
    const char* deleteText = settingsSelection == 1 ? "> Delete" : "  Delete";
    renderer.drawCenteredText(UI_10_FONT_ID, centerY + 20, defaultText);
    renderer.drawCenteredText(UI_10_FONT_ID, centerY + 40, deleteText);

    const auto labels = mappedInput.mapLabels("Cancel", "Confirm", "", "");
    renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    const auto& credentials = WIFI_STORE.getCredentials();

    const auto labels = mappedInput.mapLabels("« Back", selectorIndex == 0 ? "Add" : "Settings", "", "");
    renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

    const size_t totalItems = credentials.size() + 1;
    const auto pageStartIndex = selectorIndex / PAGE_ITEMS * PAGE_ITEMS;
    const std::string& defaultSSID = WIFI_STORE.getDefaultSSID();
    renderer.fillRect(0, 60 + (selectorIndex % PAGE_ITEMS) * 30 - 2, pageWidth - 1, 30);
    
    for (size_t i = pageStartIndex; i < totalItems && i < pageStartIndex + PAGE_ITEMS; i++) {
      std::string displayText;
      if (i == 0) {
        displayText = "+ Add new connection";
      } else {
        displayText = credentials[i - 1].ssid;
        if (credentials[i - 1].ssid == defaultSSID) {
          displayText += " [Default]";
        }
      }
      auto item = renderer.truncatedText(UI_10_FONT_ID, displayText.c_str(), renderer.getScreenWidth() - 40);
      renderer.drawText(UI_10_FONT_ID, 20, 60 + (i % PAGE_ITEMS) * 30, item.c_str(), i != selectorIndex);
    }
  }

  renderer.displayBuffer();
}

