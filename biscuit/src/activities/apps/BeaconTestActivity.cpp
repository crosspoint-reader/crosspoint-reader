#include "BeaconTestActivity.h"

#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_random.h>
#include <esp_wifi.h>

#include <string>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/RadioManager.h"

void BeaconTestActivity::onEnter() {
  Activity::onEnter();
  RADIO.ensureWifi();

  state = MODE_SELECT;
  mode = RANDOM;
  modeIndex = 0;
  ssids.clear();
  currentSsidIndex = 0;
  cycleCount = 0;
  lastCycleTime = 0;
  apActive = false;

  requestUpdate();
}

void BeaconTestActivity::onExit() {
  Activity::onExit();
  stopAP();
  RADIO.shutdown();
}

void BeaconTestActivity::loadSsidsForMode() {
  ssids.clear();
  switch (mode) {
    case RANDOM:
      for (int i = 0; i < 20; i++) ssids.push_back(generateRandomSsid());
      break;
    case CUSTOM:
      loadCustomSsids();
      if (ssids.empty()) ssids.push_back("biscuit.");
      break;
    case RICKROLL:
      ssids.push_back("Never Gonna Give You Up");
      ssids.push_back("Never Gonna Let You Down");
      ssids.push_back("Never Gonna Run Around");
      ssids.push_back("And Desert You");
      ssids.push_back("Never Gonna Make You Cry");
      ssids.push_back("Never Gonna Say Goodbye");
      ssids.push_back("Never Gonna Tell A Lie");
      ssids.push_back("And Hurt You");
      break;
    case FUNNY:
      ssids.push_back("FBI Surveillance Van");
      ssids.push_back("Pretty Fly for a Wi-Fi");
      ssids.push_back("Wu-Tang LAN");
      ssids.push_back("The LAN Before Time");
      ssids.push_back("Bill Wi the Science Fi");
      ssids.push_back("Abraham Linksys");
      ssids.push_back("LAN Solo");
      ssids.push_back("Silence of the LANs");
      ssids.push_back("Lord of the Pings");
      ssids.push_back("The Promised LAN");
      ssids.push_back("Drop It Like Its Hotspot");
      ssids.push_back("Get Off My LAN");
      ssids.push_back("It Burns When IP");
      ssids.push_back("Nacho WiFi");
      ssids.push_back("Router I Hardly Know Her");
      break;
  }
  currentSsidIndex = 0;
}

void BeaconTestActivity::loadCustomSsids() {
  auto file = Storage.open("/biscuit/beacons.txt");
  if (!file || file.isDirectory()) {
    if (file) file.close();
    LOG_DBG("Beacon", "No custom SSIDs at /biscuit/beacons.txt");
    return;
  }

  char line[64];
  while (file.available() && ssids.size() < 50) {
    int len = 0;
    char c;
    while (file.available() && len < 63) {
      c = file.read();
      if (c == '\n') break;
      if (c != '\r') line[len++] = c;
    }
    line[len] = '\0';
    if (len > 0) {
      ssids.emplace_back(line);
    }
  }
  file.close();
  LOG_DBG("Beacon", "Loaded %zu custom SSIDs", ssids.size());
}

std::string BeaconTestActivity::generateRandomSsid() {
  static const char cs[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
  int len = 6 + (esp_random() % 10);
  std::string r;
  r.reserve(len);
  for (int i = 0; i < len; i++) r += cs[esp_random() % (sizeof(cs) - 1)];
  return r;
}

void BeaconTestActivity::startAP(const std::string& ssid) {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid.c_str(), nullptr, 1 + (esp_random() % 11), 0, 0);
  apActive = true;
  LOG_DBG("Beacon", "AP started: %s", ssid.c_str());
}

void BeaconTestActivity::stopAP() {
  if (apActive) {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    apActive = false;
  }
}

void BeaconTestActivity::cycleNext() {
  if (ssids.empty()) return;
  currentSsidIndex = (currentSsidIndex + 1) % ssids.size();
  cycleCount++;

  // Regenerate random SSIDs each full cycle
  if (mode == RANDOM && currentSsidIndex == 0) {
    ssids.clear();
    for (int i = 0; i < 20; i++) ssids.push_back(generateRandomSsid());
  }

  WiFi.softAPdisconnect(true);
  WiFi.softAP(ssids[currentSsidIndex].c_str(), nullptr,
              1 + (esp_random() % 11), 0, 0);
}

void BeaconTestActivity::loop() {
  if (state == MODE_SELECT) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      mode = static_cast<Mode>(modeIndex);
      loadSsidsForMode();
      startAP(ssids[0]);
      lastCycleTime = millis();
      state = RUNNING;
      requestUpdate();
      return;
    }

    buttonNavigator.onNext([this] {
      modeIndex = ButtonNavigator::nextIndex(modeIndex, 4);
      requestUpdate();
    });

    buttonNavigator.onPrevious([this] {
      modeIndex = ButtonNavigator::previousIndex(modeIndex, 4);
      requestUpdate();
    });
    return;
  }

  // RUNNING state
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    stopAP();
    state = MODE_SELECT;
    requestUpdate();
    return;
  }

  if (millis() - lastCycleTime >= CYCLE_INTERVAL_MS) {
    cycleNext();
    lastCycleTime = millis();
    requestUpdate();
  }
}

void BeaconTestActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Beacon Spam");

  if (state == MODE_SELECT) {
    const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

    static const char* modeNames[] = {"Random", "Custom (SD)", "Rickroll", "Funny"};

    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, 4, modeIndex,
        [](int index) { return std::string(modeNames[index]); });

    const auto labels = mappedInput.mapLabels("Back", "Select", "^", "v");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    // RUNNING display
    const int centerY = pageHeight / 2 - 50;

    static const char* modeLabels[] = {"RANDOM", "CUSTOM", "RICKROLL", "FUNNY"};
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, modeLabels[mode]);

    // Current SSID (large, bold)
    if (!ssids.empty() && currentSsidIndex < static_cast<int>(ssids.size())) {
      std::string displaySsid = ssids[currentSsidIndex];
      if (displaySsid.length() > 28) {
        displaySsid = displaySsid.substr(0, 25) + "...";
      }
      renderer.drawCenteredText(UI_12_FONT_ID, centerY + 35, displaySsid.c_str(), true, EpdFontFamily::BOLD);
    }

    // Stats
    char buf[64];
    snprintf(buf, sizeof(buf), "SSID %d/%d  Cycle %d",
             currentSsidIndex + 1, static_cast<int>(ssids.size()), cycleCount);
    renderer.drawCenteredText(UI_10_FONT_ID, centerY + 70, buf);

    snprintf(buf, sizeof(buf), "Interval: %lums", CYCLE_INTERVAL_MS);
    renderer.drawCenteredText(UI_10_FONT_ID, centerY + 95, buf);

    const auto labels = mappedInput.mapLabels("Stop", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
