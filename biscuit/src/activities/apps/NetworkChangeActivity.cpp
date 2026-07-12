#include "NetworkChangeActivity.h"

#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>
#include <string.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/RadioManager.h"

// ---------------------------------------------------------------------------
// onEnter / onExit
// ---------------------------------------------------------------------------

void NetworkChangeActivity::onEnter() {
  Activity::onEnter();
  state = MENU;
  menuIndex = 0;
  newDevices.clear();
  goneDevices.clear();
  current.devices.clear();
  saved.devices.clear();
  requestUpdate();
}

void NetworkChangeActivity::onExit() {
  Activity::onExit();
  RADIO.shutdown();
}

// ---------------------------------------------------------------------------
// Snapshot helpers
// ---------------------------------------------------------------------------

void NetworkChangeActivity::takeSnapshot() {
  current.devices.clear();
  current.timestamp = millis();
  snprintf(current.label, sizeof(current.label), "snap_%lu", current.timestamp);

  RADIO.ensureWifi();
  WiFi.disconnect();

  int found = WiFi.scanNetworks(false, true);
  if (found > 0) {
    for (int i = 0; i < found && static_cast<int>(current.devices.size()) < MAX_DEVICES; i++) {
      Device d{};
      const String ssid = WiFi.SSID(i);
      strncpy(d.name, ssid.c_str(), sizeof(d.name) - 1);
      d.name[sizeof(d.name) - 1] = '\0';

      const uint8_t* bssid = WiFi.BSSID(i);
      snprintf(d.mac, sizeof(d.mac), "%02X:%02X:%02X:%02X:%02X:%02X",
               bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
      d.rssi = static_cast<int8_t>(WiFi.RSSI(i));
      d.type = 0;
      current.devices.push_back(d);
    }
  }
  WiFi.scanDelete();
}

void NetworkChangeActivity::saveSnapshotToFile(const Snapshot& snap) {
  Storage.mkdir("/biscuit");
  Storage.mkdir(SNAPSHOT_DIR);

  char path[64];
  snprintf(path, sizeof(path), "%s/snap_%lu.dat", SNAPSHOT_DIR, snap.timestamp);

  auto file = Storage.open(path, O_WRITE | O_CREAT | O_TRUNC);
  if (!file) {
    LOG_ERR("NETCHANGE", "Failed to open %s for write", path);
    return;
  }

  const uint16_t count = static_cast<uint16_t>(snap.devices.size());
  file.write(reinterpret_cast<const uint8_t*>(&count), sizeof(count));
  for (const auto& d : snap.devices) {
    file.write(reinterpret_cast<const uint8_t*>(&d), sizeof(Device));
  }
  const uint32_t ts = static_cast<uint32_t>(snap.timestamp);
  file.write(reinterpret_cast<const uint8_t*>(&ts), sizeof(ts));
  file.write(reinterpret_cast<const uint8_t*>(snap.label), sizeof(snap.label));
  file.flush();
  file.close();
  LOG_DBG("NETCHANGE", "Saved snapshot %s (%u devices)", path, count);
}

bool NetworkChangeActivity::loadSnapshot() {
  // Find the most-recently-modified .dat file by scanning directory.
  // The Storage API may not support directory iteration; we instead keep
  // track of the latest by a known naming pattern.  We scan by trying the
  // newest naming convention (the file we just saved is named snap_<ts>.dat).
  // Since we cannot list the directory easily, we save a fixed "last.dat"
  // as well as the timestamped file.
  char path[64];
  snprintf(path, sizeof(path), "%s/last.dat", SNAPSHOT_DIR);

  auto file = Storage.open(path, O_READ);
  if (!file) {
    LOG_ERR("NETCHANGE", "No saved snapshot found at %s", path);
    return false;
  }

  uint16_t count = 0;
  file.read(reinterpret_cast<uint8_t*>(&count), sizeof(count));
  if (count > static_cast<uint16_t>(MAX_DEVICES)) count = static_cast<uint16_t>(MAX_DEVICES);

  saved.devices.clear();
  saved.devices.reserve(count);
  for (uint16_t i = 0; i < count; i++) {
    Device d{};
    file.read(reinterpret_cast<uint8_t*>(&d), sizeof(Device));
    saved.devices.push_back(d);
  }
  uint32_t ts = 0;
  file.read(reinterpret_cast<uint8_t*>(&ts), sizeof(ts));
  saved.timestamp = ts;
  file.read(reinterpret_cast<uint8_t*>(saved.label), sizeof(saved.label));
  file.close();
  return true;
}

void NetworkChangeActivity::compareSnapshots() {
  newDevices.clear();
  goneDevices.clear();

  // Devices in current but not in saved → NEW
  for (const auto& cd : current.devices) {
    bool found = false;
    for (const auto& sd : saved.devices) {
      if (strcmp(cd.mac, sd.mac) == 0) { found = true; break; }
    }
    if (!found) newDevices.push_back(cd);
  }

  // Devices in saved but not in current → GONE
  for (const auto& sd : saved.devices) {
    bool found = false;
    for (const auto& cd : current.devices) {
      if (strcmp(cd.mac, sd.mac) == 0) { found = true; break; }
    }
    if (!found) goneDevices.push_back(sd);
  }
}

// ---------------------------------------------------------------------------
// loop
// ---------------------------------------------------------------------------

void NetworkChangeActivity::loop() {
  if (state == TAKING_SNAPSHOT) {
    if (millis() - lastSpinnerUpdate >= 600) {
      lastSpinnerUpdate = millis();
      spinnerFrame = (spinnerFrame + 1) % 3;
      requestUpdate();
    }
    // Blocking in loop() is acceptable for a one-shot scan; render showed
    // "Scanning..." before we entered this state.
    takeSnapshot();

    // Also save as "last.dat" so loadSnapshot() can retrieve it.
    {
      Snapshot tmp = current;
      strncpy(tmp.label, "last", sizeof(tmp.label));
      char path[64];
      snprintf(path, sizeof(path), "%s/last.dat", SNAPSHOT_DIR);
      Storage.mkdir("/biscuit");
      Storage.mkdir(SNAPSHOT_DIR);
      auto file = Storage.open(path, O_WRITE | O_CREAT | O_TRUNC);
      if (file) {
        const uint16_t cnt = static_cast<uint16_t>(tmp.devices.size());
        file.write(reinterpret_cast<const uint8_t*>(&cnt), sizeof(cnt));
        for (const auto& d : tmp.devices) {
          file.write(reinterpret_cast<const uint8_t*>(&d), sizeof(Device));
        }
        const uint32_t ts = static_cast<uint32_t>(tmp.timestamp);
        file.write(reinterpret_cast<const uint8_t*>(&ts), sizeof(ts));
        file.write(reinterpret_cast<const uint8_t*>(tmp.label), sizeof(tmp.label));
        file.flush();
        file.close();
      }
    }
    // Also save timestamped copy
    saveSnapshotToFile(current);
    RADIO.shutdown();

    state = MENU;
    menuIndex = 0;
    requestUpdate();
    return;
  }

  if (state == COMPARING) {
    if (millis() - lastSpinnerUpdate >= 600) {
      lastSpinnerUpdate = millis();
      spinnerFrame = (spinnerFrame + 1) % 3;
      requestUpdate();
    }
    if (!loadSnapshot()) {
      state = MENU;
      requestUpdate();
      return;
    }
    takeSnapshot();
    compareSnapshots();
    RADIO.shutdown();
    resultIndex = 0;
    state = RESULTS;
    requestUpdate();
    return;
  }

  if (state == RESULTS) {
    const int total = static_cast<int>(newDevices.size() + goneDevices.size());

    buttonNavigator.onNext([this, total] {
      if (total > 0) {
        resultIndex = ButtonNavigator::nextIndex(resultIndex, total);
        requestUpdate();
      }
    });
    buttonNavigator.onPrevious([this, total] {
      if (total > 0) {
        resultIndex = ButtonNavigator::previousIndex(resultIndex, total);
        requestUpdate();
      }
    });

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = MENU;
      requestUpdate();
    }
    return;
  }

  // MENU state
  buttonNavigator.onNext([this] {
    menuIndex = ButtonNavigator::nextIndex(menuIndex, MENU_COUNT);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this] {
    menuIndex = ButtonNavigator::previousIndex(menuIndex, MENU_COUNT);
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    switch (menuIndex) {
      case 0:
        state = TAKING_SNAPSHOT;
        requestUpdate();
        break;
      case 1:
        state = COMPARING;
        requestUpdate();
        break;
      case 2:
        finish();
        break;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
  }
}

// ---------------------------------------------------------------------------
// render
// ---------------------------------------------------------------------------

void NetworkChangeActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  if (state == TAKING_SNAPSHOT || state == COMPARING) {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                   "Network Change");
    GUI.drawSpinner(renderer, pageWidth / 2, pageHeight / 2, "SCANNING...", spinnerFrame);
    renderer.displayBuffer();
    return;
  }

  if (state == MENU) {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                   "Network Change");

    const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

    GUI.drawList(renderer, Rect{0, contentTop, pageWidth, contentHeight},
                 MENU_COUNT, menuIndex,
                 [](int i) -> std::string { return NetworkChangeActivity::MENU_ITEMS[i]; });

    const auto labels = mappedInput.mapLabels("Back", "Select", "^", "v");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // RESULTS
  const int newCount = static_cast<int>(newDevices.size());
  const int goneCount = static_cast<int>(goneDevices.size());
  const int total = newCount + goneCount;

  char subtitle[32];
  snprintf(subtitle, sizeof(subtitle), "+%d / -%d devices", newCount, goneCount);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 "Network Change", subtitle);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (total == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, "No changes detected");
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight},
        total, resultIndex,
        [this, newCount](int i) -> std::string {
          if (i < newCount) {
            char buf[40];
            snprintf(buf, sizeof(buf), "[NEW] %s", newDevices[i].name[0] ? newDevices[i].name : "(hidden)");
            return std::string(buf);
          }
          int gi = i - newCount;
          char buf[40];
          snprintf(buf, sizeof(buf), "[GONE] %s", goneDevices[gi].name[0] ? goneDevices[gi].name : "(hidden)");
          return std::string(buf);
        },
        [this, newCount](int i) -> std::string {
          const Device& d = (i < newCount) ? newDevices[i] : goneDevices[i - newCount];
          char buf[32];
          snprintf(buf, sizeof(buf), "%s  %ddBm", d.mac, static_cast<int>(d.rssi));
          return std::string(buf);
        });
  }

  const auto labels = mappedInput.mapLabels("Back", "", "^", "v");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
