#include "CredentialSettingsActivity.h"

#include <GfxRenderer.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "fontIds.h"

namespace {
constexpr int FIELD_COUNT = 6;
const char* FIELD_NAMES[FIELD_COUNT] = {
    "FTP Username",
    "FTP Password",
    "HTTP Username",
    "HTTP Password",
    "Hotspot SSID",
    "Hotspot Password"
};
}  // namespace

void CredentialSettingsActivity::taskTrampoline(void* param) {
  auto* self = static_cast<CredentialSettingsActivity*>(param);
  self->displayTaskLoop();
}

void CredentialSettingsActivity::onEnter() {
  Activity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();
  selectedIndex = 0;
  updateRequired = true;

  xTaskCreate(&CredentialSettingsActivity::taskTrampoline, "CredentialSettingsTask",
              2048,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void CredentialSettingsActivity::onExit() {
  ActivityWithSubactivity::onExit();

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void CredentialSettingsActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    SETTINGS.saveToFile();
    onGoBack();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    selectCurrentField();
    return;
  }

  // Handle navigation
  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    selectedIndex = (selectedIndex > 0) ? (selectedIndex - 1) : (FIELD_COUNT - 1);
    updateRequired = true;
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
             mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    selectedIndex = (selectedIndex + 1) % FIELD_COUNT;
    updateRequired = true;
  }
}

void CredentialSettingsActivity::selectCurrentField() {
  std::string* targetField = nullptr;
  const char* promptText = "";
  
  switch (selectedIndex) {
    case 0:  // FTP Username
      targetField = &SETTINGS.ftpUsername;
      promptText = "Enter FTP username:";
      break;
    case 1:  // FTP Password
      targetField = &SETTINGS.ftpPassword;
      promptText = "Enter FTP password:";
      break;
    case 2:  // HTTP Username
      targetField = &SETTINGS.httpUsername;
      promptText = "Enter HTTP username:";
      break;
    case 3:  // HTTP Password
      targetField = &SETTINGS.httpPassword;
      promptText = "Enter HTTP password:";
      break;
    case 4:  // Hotspot SSID
      targetField = &SETTINGS.apSsid;
      promptText = "Enter hotspot SSID:";
      break;
    case 5:  // Hotspot Password
      targetField = &SETTINGS.apPassword;
      promptText = "Enter hotspot password (leave empty for open network):";
      break;
  }

  if (targetField) {
    xSemaphoreTake(renderingMutex, portMAX_DELAY);
    exitActivity();
    bool isPassword = (selectedIndex == 1 || selectedIndex == 3 || selectedIndex == 5); // Password fields
    enterNewActivity(new KeyboardEntryActivity(
        renderer, mappedInput, 
        promptText,        // title
        *targetField,      // initialText
        10,                // startY
        0,                 // maxLength (0 = unlimited)
        isPassword,        // isPassword
        [this, targetField](const std::string& newValue) {
          *targetField = newValue;
          SETTINGS.saveToFile();
          exitActivity();
          updateRequired = true;
        },
        [this] {
          exitActivity();
          updateRequired = true;
        }));
    xSemaphoreGive(renderingMutex);
  }
}

void CredentialSettingsActivity::displayTaskLoop() {
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

void CredentialSettingsActivity::render() const {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Draw header
  renderer.drawCenteredText(UI_12_FONT_ID, 15, "Network Credentials", true, BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, 40, "Configure server and hotspot credentials");

  // Draw selection
  renderer.fillRect(0, 70 + selectedIndex * 35 - 2, pageWidth - 1, 35);

  // Draw fields
  for (int i = 0; i < FIELD_COUNT; i++) {
    const int fieldY = 70 + i * 35;
    const bool isSelected = (i == selectedIndex);

    // Draw field name
    renderer.drawText(UI_10_FONT_ID, 20, fieldY, FIELD_NAMES[i], !isSelected);

    // Draw current value (masked for passwords)
    std::string displayValue;
    switch (i) {
      case 0:  // FTP Username
        displayValue = SETTINGS.ftpUsername;
        break;
      case 1:  // FTP Password
        displayValue = SETTINGS.ftpPassword.empty() ? "" : std::string(SETTINGS.ftpPassword.length(), '*');
        break;
      case 2:  // Hotspot SSID
        displayValue = SETTINGS.apSsid;
        break;
      case 3:  // Hotspot Password
        displayValue = SETTINGS.apPassword.empty() ? "(open)" : std::string(SETTINGS.apPassword.length(), '*');
        break;
    }

    const auto width = renderer.getTextWidth(UI_10_FONT_ID, displayValue.c_str());
    renderer.drawText(UI_10_FONT_ID, pageWidth - 20 - width, fieldY, displayValue.c_str(), !isSelected);
  }

  // Draw help text
  const auto labels = mappedInput.mapLabels("« Save", "Edit", "", "");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
