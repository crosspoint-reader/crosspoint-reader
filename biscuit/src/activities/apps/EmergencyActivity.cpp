#include "EmergencyActivity.h"

#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include <cstring>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/RadioManager.h"

#define RADIO RadioManager::getInstance()

static constexpr uint8_t ESPNOW_CHANNEL = 1;
static constexpr uint8_t FRAME_SOS = 0x03;

// ----------------------------------------------------------------
// Constexpr definitions
// ----------------------------------------------------------------

constexpr unsigned long EmergencyActivity::INTERVALS[];

// ----------------------------------------------------------------
// Medical info
// ----------------------------------------------------------------

void EmergencyActivity::loadMedicalInfo() {
  memset(&medInfo, 0, sizeof(medInfo));
  auto file = Storage.open("/biscuit/medical.dat");
  if (file) {
    // MedicalInfo layout: name[32], bloodType[8], allergies[128],
    //   medications[128], conditions[128], emergencyContact[64], emergencyPhone[20]
    // Total: 508 bytes. We only need name, bloodType, allergies, emergencyContact, emergencyPhone.
    uint8_t buf[508];
    memset(buf, 0, sizeof(buf));
    if (file.read(buf, 508) == 508) {
      memcpy(medInfo.name, buf, 32);
      memcpy(medInfo.bloodType, buf + 32, 8);
      memcpy(medInfo.allergies, buf + 40, 128);
      memcpy(medInfo.emergencyContact, buf + 296, 64);
      memcpy(medInfo.emergencyPhone, buf + 360, 20);
      medInfoLoaded = true;
    }
    file.close();
  }
}

// ----------------------------------------------------------------
// ESP-NOW mesh SOS
// ----------------------------------------------------------------

void EmergencyActivity::sendMeshSos() {
  if (!espnowInitialized) return;

  // Frame: [type:1][mac:6][name:32][emergencyContact:32][emergencyPhone:20] = 91 bytes
  uint8_t frame[91] = {};
  frame[0] = FRAME_SOS;

  uint8_t localMac[6] = {};
  WiFi.macAddress(localMac);
  memcpy(frame + 1, localMac, 6);

  const char* nameStr = (medInfoLoaded && medInfo.name[0]) ? medInfo.name : "Unknown";
  strncpy(reinterpret_cast<char*>(frame + 7), nameStr, 31);

  if (medInfoLoaded) {
    strncpy(reinterpret_cast<char*>(frame + 39), medInfo.emergencyContact, 31);
    strncpy(reinterpret_cast<char*>(frame + 71), medInfo.emergencyPhone, 19);
  }

  uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  esp_now_send(broadcast, frame, sizeof(frame));
  lastMeshSos = millis();
}

// ----------------------------------------------------------------
// Emergency trigger / stop
// ----------------------------------------------------------------

void EmergencyActivity::triggerEmergency() {
  if (sosActive) return;

  sosActive = true;
  triggerTime = millis();
  state = TRIGGERED;

  if (broadcastMesh) {
    // Use AP+STA so WiFi AP and ESP-NOW coexist
    RADIO.ensureWifi();
    WiFi.mode(WIFI_AP_STA);
    WiFi.disconnect();
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

    if (esp_now_init() == ESP_OK) {
      espnowInitialized = true;
      esp_now_peer_info_t peer = {};
      memset(peer.peer_addr, 0xFF, 6);
      peer.channel = ESPNOW_CHANNEL;
      peer.encrypt = false;
      esp_now_add_peer(&peer);
      LOG_DBG("EMERG", "ESP-NOW initialized for SOS mesh");
    } else {
      LOG_ERR("EMERG", "ESP-NOW init failed");
    }
  }

  if (broadcastWifi) {
    char ssid[40];
    if (medInfoLoaded && medInfo.name[0]) {
      // Sanitize name: replace spaces with dashes, limit to 20 chars
      char safeName[21] = {};
      strncpy(safeName, medInfo.name, 20);
      for (int i = 0; safeName[i]; i++) {
        if (safeName[i] == ' ') safeName[i] = '-';
      }
      snprintf(ssid, sizeof(ssid), "SOS-%s", safeName);
    } else {
      snprintf(ssid, sizeof(ssid), "SOS-BISCUIT");
    }

    if (broadcastMesh) {
      // Already set AP+STA above; configure AP
      WiFi.softAP(ssid);
    } else {
      RADIO.ensureWifi();
      WiFi.mode(WIFI_AP);
      WiFi.softAP(ssid);
    }
    LOG_DBG("EMERG", "SOS WiFi AP started: %s", ssid);
  }

  // Send first mesh SOS immediately
  if (broadcastMesh && espnowInitialized) {
    sendMeshSos();
  }

  requestUpdate();
}

void EmergencyActivity::stopEmergency() {
  if (broadcastWifi) {
    WiFi.softAPdisconnect(true);
  }
  if (espnowInitialized) {
    esp_now_deinit();
    espnowInitialized = false;
  }
  RADIO.shutdown();
  sosActive = false;
}

// ----------------------------------------------------------------
// Lifecycle
// ----------------------------------------------------------------

void EmergencyActivity::onEnter() {
  Activity::onEnter();
  loadMedicalInfo();
  state = CONFIG;
  configIndex = 0;
  confirmPressCount = 0;
  lastConfirmTime = 0;
  missedCheckIns = 0;
  sosActive = false;
  espnowInitialized = false;
  requestUpdate();
}

void EmergencyActivity::onExit() {
  Activity::onExit();
  if (sosActive) stopEmergency();
}

// ----------------------------------------------------------------
// Loop
// ----------------------------------------------------------------

void EmergencyActivity::loop() {
  switch (state) {
    // ---- CONFIG ----
    case CONFIG: {
      buttonNavigator.onNext([this] {
        configIndex = ButtonNavigator::nextIndex(configIndex, CONFIG_ITEMS);
        requestUpdate();
      });
      buttonNavigator.onPrevious([this] {
        configIndex = ButtonNavigator::previousIndex(configIndex, CONFIG_ITEMS);
        requestUpdate();
      });

      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        switch (configIndex) {
          case 0:  // Mode
            mode = (mode == PANIC_MANUAL) ? DEAD_MAN_SWITCH : PANIC_MANUAL;
            requestUpdate();
            break;
          case 1:  // Interval
            intervalIndex = (intervalIndex + 1) % INTERVAL_COUNT;
            requestUpdate();
            break;
          case 2:  // WiFi SOS
            broadcastWifi = !broadcastWifi;
            requestUpdate();
            break;
          case 3:  // Mesh SOS
            broadcastMesh = !broadcastMesh;
            requestUpdate();
            break;
          case 4:  // Show Medical
            showMedical = !showMedical;
            requestUpdate();
            break;
          case 5:  // ARM
            state = ARMED;
            lastCheckIn = millis();
            missedCheckIns = 0;
            confirmPressCount = 0;
            lastConfirmTime = 0;
            requestUpdate();
            break;
        }
      }

      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        finish();
      }
      break;
    }

    // ---- ARMED ----
    case ARMED: {
      if (mode == PANIC_MANUAL) {
        // Triple-press Confirm within 1 second → trigger
        if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
          unsigned long now = millis();
          if (confirmPressCount > 0 && now - lastConfirmTime > 1000) {
            // Window expired, restart count
            confirmPressCount = 0;
          }
          confirmPressCount++;
          lastConfirmTime = now;
          if (confirmPressCount >= 3) {
            triggerEmergency();
            return;
          }
          requestUpdate();
        }
        // Reset counter if window expired
        if (confirmPressCount > 0 && millis() - lastConfirmTime > 1000) {
          confirmPressCount = 0;
          requestUpdate();
        }
        if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
          state = CONFIG;
          confirmPressCount = 0;
          requestUpdate();
        }
      } else {
        // Dead man's switch
        if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
          // Check-in: reset timer
          lastCheckIn = millis();
          missedCheckIns = 0;
          requestUpdate();
        }
        // Check if interval elapsed
        if (millis() - lastCheckIn >= INTERVALS[intervalIndex]) {
          missedCheckIns++;
          if (missedCheckIns >= MAX_MISSED) {
            triggerEmergency();
            return;
          }
          state = CHECK_IN;
          lastCheckIn = millis();  // reset so CHECK_IN doesn't immediately re-fire
          requestUpdate();
        }
        if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
          state = CONFIG;
          requestUpdate();
        }
      }
      break;
    }

    // ---- CHECK_IN ----
    case CHECK_IN: {
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        missedCheckIns = 0;
        lastCheckIn = millis();
        state = ARMED;
        requestUpdate();
      }
      // If another interval elapses without check-in, count another miss
      if (millis() - lastCheckIn >= INTERVALS[intervalIndex]) {
        missedCheckIns++;
        lastCheckIn = millis();
        if (missedCheckIns >= MAX_MISSED) {
          triggerEmergency();
          return;
        }
        requestUpdate();
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        state = CONFIG;
        missedCheckIns = 0;
        requestUpdate();
      }
      break;
    }

    // ---- TRIGGERED ----
    case TRIGGERED: {
      // Periodic mesh SOS broadcasts
      if (broadcastMesh && espnowInitialized &&
          millis() - lastMeshSos >= MESH_SOS_INTERVAL_MS) {
        sendMeshSos();
      }

      // Cancel: hold Back for 3 seconds
      if (mappedInput.isPressed(MappedInputManager::Button::Back) &&
          mappedInput.getHeldTime() >= 3000) {
        stopEmergency();
        state = CONFIG;
        confirmPressCount = 0;
        missedCheckIns = 0;
        requestUpdate();
        return;
      }

      // Refresh display periodically to update elapsed time
      requestUpdate();
      break;
    }
  }
}

// ----------------------------------------------------------------
// Render dispatch
// ----------------------------------------------------------------

void EmergencyActivity::render(RenderLock&&) {
  renderer.clearScreen();
  switch (state) {
    case CONFIG:    renderConfig();    break;
    case ARMED:     renderArmed();     break;
    case CHECK_IN:  renderCheckIn();   break;
    case TRIGGERED: renderTriggered(); break;
  }
  renderer.displayBuffer();
}

// ----------------------------------------------------------------
// renderConfig
// ----------------------------------------------------------------

void EmergencyActivity::renderConfig() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 "Emergency", "Configure & arm");

  const int listTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int listH = pageHeight - listTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  GUI.drawList(
      renderer, Rect{0, listTop, pageWidth, listH},
      CONFIG_ITEMS, configIndex,
      [this](int i) -> std::string {
        switch (i) {
          case 0: return (mode == PANIC_MANUAL) ? "Mode: Manual Panic" : "Mode: Dead Man's Switch";
          case 1: {
            char buf[32];
            unsigned long ms = INTERVALS[intervalIndex];
            unsigned long mins = ms / 60000;
            snprintf(buf, sizeof(buf), "Check-in interval: %lum", mins);
            return buf;
          }
          case 2: return broadcastWifi ? "WiFi SOS: ON" : "WiFi SOS: OFF";
          case 3: return broadcastMesh ? "Mesh SOS: ON" : "Mesh SOS: OFF";
          case 4: return showMedical ? "Show Medical: ON" : "Show Medical: OFF";
          case 5: return "ARM";
          default: return "";
        }
      },
      [this](int i) -> std::string {
        switch (i) {
          case 0: return (mode == PANIC_MANUAL)
              ? "Triple-press Confirm to trigger"
              : "Auto-trigger if check-ins missed";
          case 1: return (mode == DEAD_MAN_SWITCH) ? "Dead man mode only" : "N/A for manual mode";
          case 2: return "Create SOS WiFi access point";
          case 3: return "Broadcast via ESP-NOW mesh";
          case 4: return medInfoLoaded ? "Medical data loaded" : "No medical data on file";
          case 5: return "Activate emergency monitoring";
          default: return "";
        }
      });

  const auto labels = mappedInput.mapLabels("Back", "Select", "^", "v");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

// ----------------------------------------------------------------
// renderArmed
// ----------------------------------------------------------------

void EmergencyActivity::renderArmed() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 "ARMED", "Emergency monitoring active");

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int centerY = contentTop + (pageHeight - contentTop - metrics.buttonHintsHeight) / 2;

  if (mode == PANIC_MANUAL) {
    renderer.drawCenteredText(UI_12_FONT_ID, centerY - 30,
                              "Triple-press Confirm for SOS", true, EpdFontFamily::BOLD);
    if (confirmPressCount > 0) {
      char buf[32];
      snprintf(buf, sizeof(buf), "Presses: %d / 3", confirmPressCount);
      renderer.drawCenteredText(UI_10_FONT_ID, centerY + 10, buf);
    }
  } else {
    // Countdown to next check-in
    unsigned long elapsed = millis() - lastCheckIn;
    unsigned long interval = INTERVALS[intervalIndex];
    unsigned long remaining = (elapsed < interval) ? (interval - elapsed) : 0;
    unsigned long remSec = remaining / 1000;
    unsigned long mins = remSec / 60;
    unsigned long secs = remSec % 60;

    char countBuf[32];
    snprintf(countBuf, sizeof(countBuf), "Next check-in in: %02lu:%02lu", mins, secs);
    renderer.drawCenteredText(UI_12_FONT_ID, centerY - 30, countBuf, true, EpdFontFamily::BOLD);

    char missBuf[32];
    snprintf(missBuf, sizeof(missBuf), "Missed: %d of %d", missedCheckIns, MAX_MISSED);
    renderer.drawCenteredText(UI_10_FONT_ID, centerY + 10, missBuf);

    renderer.drawCenteredText(SMALL_FONT_ID, centerY + 40, "Press Confirm to check in");
  }

  const auto labels = mappedInput.mapLabels("Disarm", "Check-in", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

// ----------------------------------------------------------------
// renderCheckIn
// ----------------------------------------------------------------

void EmergencyActivity::renderCheckIn() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Flashing-style: alternate header text based on elapsed seconds
  unsigned long sec = millis() / 1000;
  const char* headerTitle = (sec % 2 == 0) ? "! CHECK IN NOW !" : "CHECK IN REQUIRED";

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 headerTitle, "Press Confirm immediately");

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int centerY = contentTop + (pageHeight - contentTop - metrics.buttonHintsHeight) / 2;

  renderer.drawCenteredText(UI_12_FONT_ID, centerY - 40,
                            "MISSED CHECK-IN", true, EpdFontFamily::BOLD);

  char missBuf[40];
  snprintf(missBuf, sizeof(missBuf), "Missed: %d of %d", missedCheckIns, MAX_MISSED);
  renderer.drawCenteredText(UI_10_FONT_ID, centerY, missBuf);

  renderer.drawCenteredText(SMALL_FONT_ID, centerY + 35,
                            "Confirm to resume  |  Back to disarm");

  const auto labels = mappedInput.mapLabels("Disarm", "Check In", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

// ----------------------------------------------------------------
// renderTriggered
// ----------------------------------------------------------------

void EmergencyActivity::renderTriggered() const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  // Double border for urgency
  constexpr int BORDER = 4;
  renderer.fillRect(0, 0, pageWidth, BORDER, true);
  renderer.fillRect(0, pageHeight - BORDER, pageWidth, BORDER, true);
  renderer.fillRect(0, 0, BORDER, pageHeight, true);
  renderer.fillRect(pageWidth - BORDER, 0, BORDER, pageHeight, true);

  // SOS header band
  constexpr int HEADER_H = 56;
  renderer.fillRect(0, 0, pageWidth, HEADER_H, true);
  renderer.drawCenteredText(UI_12_FONT_ID, 14, "! SOS ACTIVE !", false, EpdFontFamily::BOLD);

  // Elapsed time
  unsigned long elapsed = (millis() - triggerTime) / 1000;
  unsigned long hrs = elapsed / 3600;
  unsigned long mins = (elapsed % 3600) / 60;
  unsigned long secs = elapsed % 60;
  char timeBuf[24];
  if (hrs > 0) {
    snprintf(timeBuf, sizeof(timeBuf), "Active: %02lu:%02lu:%02lu", hrs, mins, secs);
  } else {
    snprintf(timeBuf, sizeof(timeBuf), "Active: %02lu:%02lu", mins, secs);
  }
  renderer.drawCenteredText(SMALL_FONT_ID, HEADER_H - 16, timeBuf, false);

  int y = HEADER_H + 10;
  const int lineH12 = renderer.getLineHeight(UI_12_FONT_ID);
  const int lineH10 = renderer.getLineHeight(UI_10_FONT_ID);
  const int lineHSm = renderer.getLineHeight(SMALL_FONT_ID);
  constexpr int PAD = 14;

  // Medical info section
  if (showMedical) {
    // Name — large and prominent
    const char* nameVal = (medInfoLoaded && medInfo.name[0]) ? medInfo.name : "Unknown";
    renderer.drawText(SMALL_FONT_ID, PAD, y, "NAME", true, EpdFontFamily::BOLD);
    y += lineHSm + 2;
    renderer.drawText(UI_12_FONT_ID, PAD, y, nameVal, true, EpdFontFamily::BOLD);
    y += lineH12 + 6;

    // Blood type — highlighted box
    const char* btVal = (medInfoLoaded && medInfo.bloodType[0]) ? medInfo.bloodType : "?";
    renderer.drawText(SMALL_FONT_ID, PAD, y, "BLOOD TYPE", true, EpdFontFamily::BOLD);
    // Draw box around blood type value
    int btW = renderer.getTextWidth(UI_12_FONT_ID, btVal, EpdFontFamily::BOLD) + 10;
    renderer.drawRect(PAD - 2, y + lineHSm + 1, btW + 4, lineH12 + 4, true);
    y += lineHSm + 4;
    renderer.drawText(UI_12_FONT_ID, PAD + 3, y, btVal, true, EpdFontFamily::BOLD);
    y += lineH12 + 8;

    // Allergies — important for first responders
    if (medInfoLoaded && medInfo.allergies[0]) {
      renderer.drawText(SMALL_FONT_ID, PAD, y, "ALLERGIES", true, EpdFontFamily::BOLD);
      y += lineHSm + 2;
      renderer.drawText(UI_10_FONT_ID, PAD, y, medInfo.allergies);
      y += lineH10 + 6;
    }

    // Separator
    renderer.fillRect(PAD, y, pageWidth - PAD * 2, 1, true);
    y += 6;

    // Emergency contact section
    renderer.drawText(SMALL_FONT_ID, PAD, y, "EMERGENCY CONTACT", true, EpdFontFamily::BOLD);
    y += lineHSm + 2;
    const char* contactVal = (medInfoLoaded && medInfo.emergencyContact[0])
        ? medInfo.emergencyContact : "—";
    renderer.drawText(UI_10_FONT_ID, PAD, y, contactVal, true, EpdFontFamily::BOLD);
    y += lineH10 + 4;

    const char* phoneVal = (medInfoLoaded && medInfo.emergencyPhone[0])
        ? medInfo.emergencyPhone : "—";
    renderer.drawText(UI_12_FONT_ID, PAD, y, phoneVal, true, EpdFontFamily::BOLD);
    y += lineH12 + 8;
  }

  // Radio status
  if (broadcastWifi || broadcastMesh) {
    renderer.fillRect(PAD, y, pageWidth - PAD * 2, 1, true);
    y += 6;

    if (broadcastWifi) {
      char ssid[40];
      if (medInfoLoaded && medInfo.name[0]) {
        char safeName[21] = {};
        strncpy(safeName, medInfo.name, 20);
        for (int i = 0; safeName[i]; i++) {
          if (safeName[i] == ' ') safeName[i] = '-';
        }
        snprintf(ssid, sizeof(ssid), "WiFi AP: SOS-%s", safeName);
      } else {
        snprintf(ssid, sizeof(ssid), "WiFi AP: SOS-BISCUIT");
      }
      renderer.drawText(SMALL_FONT_ID, PAD, y, ssid);
      y += lineHSm + 3;
    }

    if (broadcastMesh) {
      renderer.drawText(SMALL_FONT_ID, PAD, y,
                        espnowInitialized ? "Mesh broadcast: active" : "Mesh broadcast: failed");
      y += lineHSm + 3;
    }
  }

  // Cancel hint near bottom
  const int hintY = pageHeight - metrics.buttonHintsHeight - lineHSm - 8;
  renderer.drawCenteredText(SMALL_FONT_ID, hintY, "Hold Back 3s to cancel SOS");

  const auto labels = mappedInput.mapLabels("(hold) Cancel", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
