#include "ScheduleSettingsActivity.h"

#include <GfxRenderer.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "fontIds.h"

namespace {
constexpr int SETTINGS_COUNT = 6;
const char* SETTING_NAMES[SETTINGS_COUNT] = {
    "Schedule Enabled",
    "Frequency",
    "Schedule Time",
    "Auto-Shutdown",
    "Protocol",
    "Network Mode"
};
}  // namespace

void ScheduleSettingsActivity::taskTrampoline(void* param) {
  auto* self = static_cast<ScheduleSettingsActivity*>(param);
  self->displayTaskLoop();
}

void ScheduleSettingsActivity::onEnter() {
  Activity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();
  selectedIndex = 0;
  updateRequired = true;

  xTaskCreate(&ScheduleSettingsActivity::taskTrampoline, "ScheduleSettingsTask",
              2048,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void ScheduleSettingsActivity::onExit() {
  Activity::onExit();

  // Wait until not rendering to delete task
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void ScheduleSettingsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    SETTINGS.saveToFile();
    onGoBack();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    toggleCurrentSetting();
    updateRequired = true;
    return;
  }

  // Handle navigation
  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    selectedIndex = (selectedIndex > 0) ? (selectedIndex - 1) : (SETTINGS_COUNT - 1);
    updateRequired = true;
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
             mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    selectedIndex = (selectedIndex + 1) % SETTINGS_COUNT;
    updateRequired = true;
  }
}

void ScheduleSettingsActivity::toggleCurrentSetting() {
  switch (selectedIndex) {
    case 0:  // Schedule Enabled
      SETTINGS.scheduleEnabled = !SETTINGS.scheduleEnabled;
      break;
    case 1:  // Frequency
      SETTINGS.scheduleFrequency = (SETTINGS.scheduleFrequency + 1) % 7;
      break;
    case 2:  // Schedule Time (hour)
      SETTINGS.scheduleHour = (SETTINGS.scheduleHour + 1) % 24;
      break;
    case 3:  // Auto-Shutdown
      SETTINGS.scheduleAutoShutdown = (SETTINGS.scheduleAutoShutdown + 1) % 6;
      break;
    case 4:  // Protocol
      SETTINGS.scheduleProtocol = (SETTINGS.scheduleProtocol + 1) % 2;
      break;
    case 5:  // Network Mode
      SETTINGS.scheduleNetworkMode = (SETTINGS.scheduleNetworkMode + 1) % 2;
      break;
  }
  SETTINGS.saveToFile();
}

void ScheduleSettingsActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void ScheduleSettingsActivity::render() const {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Draw header
  renderer.drawCenteredText(UI_12_FONT_ID, 15, "Schedule Settings", true, BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, 40, "Auto-start file transfer server");

  // Draw selection
  renderer.fillRect(0, 70 + selectedIndex * 30 - 2, pageWidth - 1, 30);

  // Draw settings
  const char* frequencyNames[] = {"1 hour", "2 hours", "3 hours", "6 hours", "12 hours", "24 hours", "Scheduled"};
  const char* shutdownNames[] = {"5 min", "10 min", "20 min", "30 min", "60 min", "120 min"};
  const char* protocolNames[] = {"HTTP", "FTP"};
  const char* networkModeNames[] = {"Join Network", "Create Hotspot"};

  for (int i = 0; i < SETTINGS_COUNT; i++) {
    const int settingY = 70 + i * 30;
    const bool isSelected = (i == selectedIndex);

    // Draw setting name
    renderer.drawText(UI_10_FONT_ID, 20, settingY, SETTING_NAMES[i], !isSelected);

    // Draw value
    std::string valueText;
    switch (i) {
      case 0:  // Schedule Enabled
        valueText = SETTINGS.scheduleEnabled ? "ON" : "OFF";
        break;
      case 1:  // Frequency
        valueText = frequencyNames[SETTINGS.scheduleFrequency];
        break;
      case 2: {  // Schedule Time
        char timeStr[6];
        snprintf(timeStr, sizeof(timeStr), "%02d:00", SETTINGS.scheduleHour);
        valueText = timeStr;
        break;
      }
      case 3:  // Auto-Shutdown
        valueText = shutdownNames[SETTINGS.scheduleAutoShutdown];
        break;
      case 4:  // Protocol
        valueText = protocolNames[SETTINGS.scheduleProtocol];
        break;
      case 5:  // Network Mode
        valueText = networkModeNames[SETTINGS.scheduleNetworkMode];
        break;
    }

    const auto width = renderer.getTextWidth(UI_10_FONT_ID, valueText.c_str());
    renderer.drawText(UI_10_FONT_ID, pageWidth - 20 - width, settingY, valueText.c_str(), !isSelected);
  }

  // Draw info text
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 100,
                            SETTINGS.scheduleFrequency == 6 ? "Server starts at scheduled time" : "Server starts at intervals");
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 80,
                            "and auto-shuts down after timeout");

  // Draw help text
  const auto labels = mappedInput.mapLabels("« Save", "Toggle", "", "");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
