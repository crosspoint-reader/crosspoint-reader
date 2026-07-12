#include "PhoneTetherActivity.h"

#include <BLEAdvertisedDevice.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <HalStorage.h>
#include <Logging.h>
#include <strings.h>  // strncasecmp

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/RadioManager.h"

// ----------------------------------------------------------------
// Config persistence
// ----------------------------------------------------------------

static constexpr const char* CFG_PATH = "/biscuit/tether.cfg";

void PhoneTetherActivity::loadConfig() {
  auto f = Storage.open(CFG_PATH);
  if (!f) return;
  char line[64];
  while (f.available()) {
    int len = 0;
    // Read one line
    while (f.available() && len < (int)sizeof(line) - 1) {
      char c = static_cast<char>(f.read());
      if (c == '\n') break;
      if (c != '\r') line[len++] = c;
    }
    line[len] = '\0';
    if (strncmp(line, "MAC=", 4) == 0 && len >= 4 + 17) {
      strncpy(targetMac, line + 4, 17);
      targetMac[17] = '\0';
    } else if (strncmp(line, "THRESHOLD=", 10) == 0) {
      rssiThreshold = static_cast<int8_t>(atoi(line + 10));
      if (rssiThreshold > -40) rssiThreshold = -40;
      if (rssiThreshold < -100) rssiThreshold = -100;
    }
  }
  f.close();
  parseMacBytes();
}

void PhoneTetherActivity::saveConfig() {
  Storage.mkdir("/biscuit");
  auto f = Storage.open(CFG_PATH, O_WRITE | O_CREAT | O_TRUNC);
  if (!f) return;
  f.print("MAC=");
  f.println(targetMac);
  f.print("THRESHOLD=");
  f.println(static_cast<int>(rssiThreshold));
  f.close();
}

// ----------------------------------------------------------------
// MAC helpers
// ----------------------------------------------------------------

void PhoneTetherActivity::parseMacBytes() {
  // Parse "AA:BB:CC:DD:EE:FF" into macBytes[6]
  unsigned int b[6] = {};
  sscanf(targetMac, "%02x:%02x:%02x:%02x:%02x:%02x",
         &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
  for (int i = 0; i < 6; i++) macBytes[i] = static_cast<uint8_t>(b[i]);
}

void PhoneTetherActivity::buildMacString() {
  snprintf(targetMac, sizeof(targetMac), "%02X:%02X:%02X:%02X:%02X:%02X",
           macBytes[0], macBytes[1], macBytes[2], macBytes[3], macBytes[4], macBytes[5]);
}

// ----------------------------------------------------------------
// RSSI history
// ----------------------------------------------------------------

void PhoneTetherActivity::pushRssiHistory(int8_t rssi) {
  rssiHistory[historyIndex] = rssi;
  historyIndex = (historyIndex + 1) % HISTORY_SIZE;
  if (historyCount < HISTORY_SIZE) historyCount++;
}

// ----------------------------------------------------------------
// BLE scanning
// ----------------------------------------------------------------

void PhoneTetherActivity::startBleScan() {
  if (!scanInitialized) return;
  BLEScan* scan = BLEDevice::getScan();
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);
  scan->clearResults();
  scan->start(0, nullptr, true);  // non-blocking, continuous
  lastScanTime = millis();
}

void PhoneTetherActivity::processScanResults() {
  BLEScan* scan = BLEDevice::getScan();
  BLEScanResults* results = scan->getResults();
  if (!results) return;

  const int count = results->getCount();
  const unsigned long now = millis();

  for (int i = 0; i < count; i++) {
    BLEAdvertisedDevice dev = results->getDevice(i);
    String devMacArduino = dev.getAddress().toString();
    const char* devMacStr = devMacArduino.c_str();
    if (strncasecmp(devMacStr, targetMac, 17) == 0) {
      targetFound = true;
      currentRssi = static_cast<int8_t>(dev.getRSSI());
      lastSeenTime = now;
      if (dev.haveName()) {
        strncpy(targetName, dev.getName().c_str(), sizeof(targetName) - 1);
        targetName[sizeof(targetName) - 1] = '\0';
      }
      pushRssiHistory(currentRssi);
      break;
    }
  }

  scan->clearResults();
  // Restart scan immediately
  lastScanTime = now;
  scan->start(0, nullptr, true);
}

// ----------------------------------------------------------------
// Lifecycle
// ----------------------------------------------------------------

void PhoneTetherActivity::onEnter() {
  Activity::onEnter();
  state = CONFIG;
  loadConfig();
  editField = 0;
  requestUpdate();
}

void PhoneTetherActivity::onExit() {
  Activity::onExit();
  if (scanInitialized) {
    BLEScan* scan = BLEDevice::getScan();
    scan->stop();
    scanInitialized = false;
    RADIO.shutdown();
  }
  needsInit = false;
}

// ----------------------------------------------------------------
// Loop
// ----------------------------------------------------------------

void PhoneTetherActivity::loop() {
  // CONFIG state
  if (state == CONFIG) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finish();
      return;
    }

    // Left/Right: move edit field
    if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      editField = (editField > 0) ? editField - 1 : 6;
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      editField = (editField < 6) ? editField + 1 : 0;
      requestUpdate();
    }

    // Up/Down: increment/decrement field value
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      if (editField < 6) {
        macBytes[editField]++;
        buildMacString();
      } else {
        if (rssiThreshold < -40) rssiThreshold++;
      }
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      if (editField < 6) {
        macBytes[editField]--;
        buildMacString();
      } else {
        if (rssiThreshold > -100) rssiThreshold--;
      }
      requestUpdate();
    }

    // Confirm: start monitoring
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      saveConfig();
      parseMacBytes();
      // Reset monitoring state
      targetFound = false;
      currentRssi = -127;
      targetName[0] = '\0';
      lastSeenTime = 0;
      historyIndex = 0;
      historyCount = 0;
      bleError[0] = '\0';
      state = MONITORING;
      if (!scanInitialized) {
        needsInit = true;
      } else {
        startBleScan();
      }
      requestUpdate();
    }
    return;
  }

  // Initialize BLE (deferred to loop to avoid blocking onEnter)
  if (needsInit) {
    needsInit = false;
    if (!RADIO.ensureBle()) {
      strncpy(bleError, "BLE init failed", sizeof(bleError) - 1);
      bleError[sizeof(bleError) - 1] = '\0';
      state = CONFIG;
      requestUpdate();
      return;
    }
    scanInitialized = true;
    startBleScan();
    requestUpdate();
    return;
  }

  // MONITORING state
  if (state == MONITORING) {
    // Process scan results periodically
    if (scanInitialized && (millis() - lastScanTime > 2000)) {
      processScanResults();
      // Check for lost condition
      if (lastSeenTime > 0 && (millis() - lastSeenTime) > LOST_TIMEOUT_MS) {
        state = ALERT;
        requestUpdate();
        return;
      }
      requestUpdate();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      // Stop scan, go back to config
      if (scanInitialized) {
        BLEScan* scan = BLEDevice::getScan();
        scan->stop();
        scanInitialized = false;
        RADIO.shutdown();
      }
      state = CONFIG;
      requestUpdate();
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      // Go back to config without stopping — user can reconfigure
      if (scanInitialized) {
        BLEScan* scan = BLEDevice::getScan();
        scan->stop();
        scanInitialized = false;
        RADIO.shutdown();
      }
      state = CONFIG;
      requestUpdate();
      return;
    }
    return;
  }

  // ALERT state — any button returns to MONITORING
  if (state == ALERT) {
    bool anyPressed =
        mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Up) ||
        mappedInput.wasReleased(MappedInputManager::Button::Down) ||
        mappedInput.wasReleased(MappedInputManager::Button::Left) ||
        mappedInput.wasReleased(MappedInputManager::Button::Right) ||
        mappedInput.wasReleased(MappedInputManager::Button::PageForward);
    if (anyPressed) {
      // Reset last-seen so we give it another LOST_TIMEOUT window
      lastSeenTime = millis();
      state = MONITORING;
      requestUpdate();
    }
  }
}

// ----------------------------------------------------------------
// Render
// ----------------------------------------------------------------

void PhoneTetherActivity::render(RenderLock&&) {
  renderer.clearScreen();
  switch (state) {
    case CONFIG:     renderConfig();     break;
    case MONITORING: renderMonitoring(); break;
    case ALERT:      renderAlert();      break;
  }
  renderer.displayBuffer();
}

void PhoneTetherActivity::renderConfig() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 "Phone Tether");

  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + 10;

  renderer.drawText(UI_10_FONT_ID, 20, y, "Target MAC:");
  y += renderer.getTextHeight(UI_10_FONT_ID) + 12;

  // Draw each MAC byte — highlight selected field
  // Each byte occupies ~50px: "XX" (2 hex digits) + ":" separator
  const int byteW = 46;
  const int sepW = 10;
  const int startX = (pageWidth - (6 * byteW + 5 * sepW)) / 2;
  const int byteH = renderer.getTextHeight(UI_10_FONT_ID) + 8;

  for (int i = 0; i < 6; i++) {
    char buf[3];
    snprintf(buf, sizeof(buf), "%02X", macBytes[i]);
    int bx = startX + i * (byteW + sepW);

    if (editField == i) {
      // Inverted: black rect, white text
      renderer.fillRect(bx, y - 4, byteW, byteH, true);
      int tw = renderer.getTextWidth(UI_10_FONT_ID, buf);
      renderer.drawText(UI_10_FONT_ID, bx + (byteW - tw) / 2, y, buf, false);
    } else {
      renderer.drawRect(bx, y - 4, byteW, byteH, true);
      int tw = renderer.getTextWidth(UI_10_FONT_ID, buf);
      renderer.drawText(UI_10_FONT_ID, bx + (byteW - tw) / 2, y, buf, true);
    }

    // Separator ":"
    if (i < 5) {
      int sx = bx + byteW + (sepW - renderer.getTextWidth(UI_10_FONT_ID, ":")) / 2;
      renderer.drawText(UI_10_FONT_ID, sx, y, ":", true);
    }
  }

  y += byteH + metrics.verticalSpacing + 10;

  // Threshold field
  char thrBuf[16];
  snprintf(thrBuf, sizeof(thrBuf), "%d dBm", static_cast<int>(rssiThreshold));
  renderer.drawText(UI_10_FONT_ID, 20, y, "RSSI Threshold:");

  {
    const int thrFieldX = 200;
    const int thrW = renderer.getTextWidth(UI_10_FONT_ID, thrBuf) + 12;
    const int thrH = renderer.getTextHeight(UI_10_FONT_ID) + 8;

    if (editField == 6) {
      renderer.fillRect(thrFieldX, y - 4, thrW, thrH, true);
      renderer.drawText(UI_10_FONT_ID, thrFieldX + 6, y, thrBuf, false);
    } else {
      renderer.drawRect(thrFieldX, y - 4, thrW, thrH, true);
      renderer.drawText(UI_10_FONT_ID, thrFieldX + 6, y, thrBuf, true);
    }
  }

  y += renderer.getTextHeight(UI_10_FONT_ID) + metrics.verticalSpacing + 8;

  // BLE error (shown when init failed)
  if (bleError[0] != '\0') {
    renderer.drawCenteredText(UI_10_FONT_ID, y, bleError, true, EpdFontFamily::BOLD);
    y += renderer.getTextHeight(UI_10_FONT_ID) + 6;
  }

  // Hint text
  renderer.drawCenteredText(SMALL_FONT_ID, y, "Left/Right: select field  Up/Down: change value");

  // Button hints
  const auto labels = mappedInput.mapLabels("Back", "Start", "<", ">");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  GUI.drawSideButtonHints(renderer, "+", "-");
}

void PhoneTetherActivity::renderMonitoring() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  if (needsInit) {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                   "Phone Tether");
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, "Starting BLE...");
    return;
  }

  // Determine status
  const unsigned long now = millis();
  const bool lost = (lastSeenTime == 0) ||
                    ((now - lastSeenTime) > LOST_TIMEOUT_MS);
  const bool weak = !lost && targetFound && (currentRssi < rssiThreshold);

  const char* statusStr;
  if (lost) statusStr = "LOST";
  else if (weak) statusStr = "WEAK";
  else statusStr = "IN RANGE";

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 "Phone Tether", statusStr);

  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing + 8;
  const int lineH = renderer.getTextHeight(UI_10_FONT_ID) + 6;

  // Target MAC
  {
    char buf[32];
    snprintf(buf, sizeof(buf), "Target: %s", targetMac);
    renderer.drawText(UI_10_FONT_ID, 20, y, buf);
    y += lineH;
  }

  // Name (if known)
  if (targetName[0] != '\0') {
    char buf[48];
    snprintf(buf, sizeof(buf), "Name:   %s", targetName);
    renderer.drawText(UI_10_FONT_ID, 20, y, buf);
    y += lineH;
  }

  // Status
  {
    renderer.drawText(UI_10_FONT_ID, 20, y, "Status:");
    int sw = renderer.getTextWidth(UI_10_FONT_ID, "Status: ");
    renderer.drawText(UI_10_FONT_ID, 20 + sw, y, statusStr, true, EpdFontFamily::BOLD);
    y += lineH;
  }

  // RSSI
  if (targetFound) {
    char buf[40];
    char thrBuf[16];
    snprintf(thrBuf, sizeof(thrBuf), "%d", static_cast<int>(rssiThreshold));
    snprintf(buf, sizeof(buf), "RSSI:   %d dBm  (Thr: %s)",
             static_cast<int>(currentRssi), thrBuf);
    renderer.drawText(UI_10_FONT_ID, 20, y, buf);
    y += lineH;
  } else {
    renderer.drawText(UI_10_FONT_ID, 20, y, "RSSI:   --  (scanning...)");
    y += lineH;
  }

  // RSSI bar graph
  // Bars: width=2, gap=1 => each entry takes 3px. 30 entries = 90px wide.
  // Graph area height: 60px. Position near bottom of content.
  if (historyCount > 0) {
    static constexpr int GRAPH_H = 60;
    static constexpr int BAR_W = 2;
    static constexpr int BAR_GAP = 1;
    static constexpr int BAR_STEP = BAR_W + BAR_GAP;

    const int graphW = HISTORY_SIZE * BAR_STEP;
    const int graphX = (pageWidth - graphW) / 2;
    const int graphBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - 4;
    const int graphTop = graphBottom - GRAPH_H;

    // Draw graph border
    renderer.drawRect(graphX - 2, graphTop - 2, graphW + 4, GRAPH_H + 4, true);

    // Draw threshold line
    // Map rssiThreshold (-100..-40) to graph pixel height
    // Signal range: -100 (floor) to -30 (ceiling). Stronger = taller bar.
    static constexpr int SIG_FLOOR = -100;
    static constexpr int SIG_CEIL = -30;
    static constexpr int SIG_RANGE = SIG_CEIL - SIG_FLOOR;  // 70

    const int thrLineY = graphBottom -
        static_cast<int>((static_cast<int>(rssiThreshold) - SIG_FLOOR) * GRAPH_H / SIG_RANGE);
    // Draw dashed threshold line
    for (int dx = 0; dx < graphW; dx += 4) {
      renderer.drawLine(graphX + dx, thrLineY, graphX + dx + 2, thrLineY, true);
    }

    // Draw bars from oldest to newest
    for (int bar = 0; bar < historyCount; bar++) {
      // oldest entry at bar=0, drawn left to right
      int entryIdx = (historyIndex - historyCount + bar + HISTORY_SIZE) % HISTORY_SIZE;
      int8_t val = rssiHistory[entryIdx];
      int clamped = val;
      if (clamped < SIG_FLOOR) clamped = SIG_FLOOR;
      if (clamped > SIG_CEIL) clamped = SIG_CEIL;
      int barH = (clamped - SIG_FLOOR) * GRAPH_H / SIG_RANGE;
      if (barH < 1) barH = 1;
      int bx = graphX + bar * BAR_STEP;
      int by = graphBottom - barH;
      renderer.fillRect(bx, by, BAR_W, barH, true);
    }
  }

  const auto labels = mappedInput.mapLabels("Back", "Config", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void PhoneTetherActivity::renderAlert() const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Full-screen alert
  // Large bold "DEVICE LOST" centered
  const int midY = pageHeight / 2;
  const int lineH12 = renderer.getTextHeight(UI_12_FONT_ID);

  renderer.drawCenteredText(UI_12_FONT_ID, midY - lineH12 * 2 - 10,
                            "! DEVICE LOST !", true, EpdFontFamily::BOLD);

  // Last seen time
  const unsigned long elapsed = (millis() - lastSeenTime) / 1000;
  char buf[40];
  snprintf(buf, sizeof(buf), "Last seen: %lus ago", elapsed);
  renderer.drawCenteredText(UI_10_FONT_ID, midY, buf);

  // Last RSSI
  if (targetFound) {
    char rssiBuf[24];
    snprintf(rssiBuf, sizeof(rssiBuf), "Last RSSI: %d dBm", static_cast<int>(currentRssi));
    renderer.drawCenteredText(UI_10_FONT_ID, midY + renderer.getTextHeight(UI_10_FONT_ID) + 8, rssiBuf);
  }

  renderer.drawCenteredText(SMALL_FONT_ID, midY + 70, "Press any button to resume monitoring");
}
