#include "BleContactExchangeActivity.h"

#include <BLEAdvertisedDevice.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEUtils.h>
#include <GfxRenderer.h>
#include <HalStorage.h>

#include <cstring>

#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/RadioManager.h"

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void BleContactExchangeActivity::onEnter() {
  Activity::onEnter();
  state = IDLE;
  idleMenuIndex = 0;
  contactIndex = 0;
  received.clear();
  bleInitialized = false;
  loadMyContact();
  requestUpdate();
}

void BleContactExchangeActivity::onExit() {
  Activity::onExit();
  if (bleInitialized) {
    stopExchange();
  }
}

// ---------------------------------------------------------------------------
// Storage
// ---------------------------------------------------------------------------

void BleContactExchangeActivity::loadMyContact() {
  MyContactData data = {};
  auto file = Storage.open(MY_CONTACT_PATH);
  if (file) {
    file.read(reinterpret_cast<uint8_t*>(&data), sizeof(data));
    file.close();
    strncpy(myName,  data.name,  sizeof(myName)  - 1);
    strncpy(myPhone, data.phone, sizeof(myPhone) - 1);
    myName[sizeof(myName)   - 1] = '\0';
    myPhone[sizeof(myPhone) - 1] = '\0';
  }
}

void BleContactExchangeActivity::saveMyContact() {
  Storage.mkdir("/biscuit");
  MyContactData data = {};
  strncpy(data.name,  myName,  sizeof(data.name)  - 1);
  strncpy(data.phone, myPhone, sizeof(data.phone) - 1);
  auto file = Storage.open(MY_CONTACT_PATH, O_WRITE | O_CREAT | O_TRUNC);
  if (file) {
    file.write(reinterpret_cast<const uint8_t*>(&data), sizeof(data));
    file.close();
  }
}

void BleContactExchangeActivity::saveContact(const Contact& contact) {
  Storage.mkdir("/biscuit");
  auto file = Storage.open(CONTACTS_PATH, O_WRITE | O_CREAT | O_APPEND);
  if (file) {
    file.print(contact.name);
    file.print(',');
    file.print(contact.phone);
    file.print('\n');
    file.close();
  }
}

// ---------------------------------------------------------------------------
// BLE exchange
// ---------------------------------------------------------------------------

void BleContactExchangeActivity::startExchange() {
  RADIO.ensureBle();
  bleInitialized = true;

  // Build manufacturer data: company_id(2) + name(14) + phone(13) = 29 bytes
  uint8_t mfgData[MFG_TOTAL] = {};
  mfgData[0] = BISCUIT_COMPANY_ID & 0xFF;
  mfgData[1] = (BISCUIT_COMPANY_ID >> 8) & 0xFF;
  strncpy(reinterpret_cast<char*>(mfgData + MFG_NAME_OFFSET),  myName,  MFG_NAME_LEN);
  strncpy(reinterpret_cast<char*>(mfgData + MFG_PHONE_OFFSET), myPhone, MFG_PHONE_LEN);

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  BLEAdvertisementData advData;
  // Arduino BLE API expects String, not std::string
  String mfgStr;
  mfgStr.reserve(MFG_TOTAL);
  for (int i = 0; i < MFG_TOTAL; i++) mfgStr += static_cast<char>(mfgData[i]);
  advData.setManufacturerData(mfgStr);
  adv->setAdvertisementData(advData);
  adv->start();

  BLEScan* scan = BLEDevice::getScan();
  scan->setActiveScan(false);
  scan->setInterval(100);
  scan->setWindow(99);
  scan->start(0, nullptr, true);  // non-blocking continuous scan

  exchangeStart = millis();
  lastScanMs = millis();
  state = EXCHANGING;
}

void BleContactExchangeActivity::stopExchange() {
  BLEDevice::getAdvertising()->stop();
  BLEDevice::getScan()->stop();
  RADIO.shutdown();
  bleInitialized = false;
}

void BleContactExchangeActivity::pollScanResults() {
  BLEScanResults* pResults = BLEDevice::getScan()->getResults();
  if (!pResults) return;
  for (int i = 0; i < pResults->getCount(); i++) {
    BLEAdvertisedDevice dev = pResults->getDevice(i);
    if (!dev.haveManufacturerData()) continue;

    String mfgRaw = dev.getManufacturerData();
    if (static_cast<int>(mfgRaw.length()) < MFG_TOTAL) continue;

    const char* mfg = mfgRaw.c_str();
    uint16_t compId = static_cast<uint8_t>(mfg[0]) |
                      (static_cast<uint8_t>(mfg[1]) << 8);
    if (compId != BISCUIT_COMPANY_ID) continue;

    Contact c = {};
    memcpy(c.name,  mfg + MFG_NAME_OFFSET,  MFG_NAME_LEN);
    memcpy(c.phone, mfg + MFG_PHONE_OFFSET, MFG_PHONE_LEN);
    c.name[MFG_NAME_LEN]   = '\0';
    c.phone[MFG_PHONE_LEN] = '\0';

    // Skip empty / own contact
    if (c.name[0] == '\0') continue;
    if (strncmp(c.name, myName, MFG_NAME_LEN) == 0 &&
        strncmp(c.phone, myPhone, MFG_PHONE_LEN) == 0) continue;

    // Deduplicate by name
    bool found = false;
    for (const auto& existing : received) {
      if (strncmp(existing.name, c.name, MFG_NAME_LEN) == 0) {
        found = true;
        break;
      }
    }
    if (!found) {
      received.push_back(c);
      requestUpdate();
    }
  }
}

// ---------------------------------------------------------------------------
// Keyboard helpers
// ---------------------------------------------------------------------------

void BleContactExchangeActivity::editName() {
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "Your Name",
                                              myName, sizeof(myName) - 1),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          const auto& text = std::get<KeyboardResult>(result.data).text;
          strncpy(myName, text.c_str(), sizeof(myName) - 1);
          myName[sizeof(myName) - 1] = '\0';
          // Chain into phone edit
          editPhone();
        }
      });
}

void BleContactExchangeActivity::editPhone() {
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "Your Phone",
                                              myPhone, sizeof(myPhone) - 1),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          const auto& text = std::get<KeyboardResult>(result.data).text;
          strncpy(myPhone, text.c_str(), sizeof(myPhone) - 1);
          myPhone[sizeof(myPhone) - 1] = '\0';
          saveMyContact();
        }
      });
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------

void BleContactExchangeActivity::loop() {
  if (state == IDLE) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finish();
      return;
    }

    buttonNavigator.onNext([this] {
      idleMenuIndex = ButtonNavigator::nextIndex(idleMenuIndex, 2);
      requestUpdate();
    });
    buttonNavigator.onPrevious([this] {
      idleMenuIndex = ButtonNavigator::previousIndex(idleMenuIndex, 2);
      requestUpdate();
    });

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (idleMenuIndex == 0) {
        // Edit info
        editName();
      } else {
        // Start exchange — require at least a name
        if (myName[0] == '\0') {
          // Redirect to edit first
          editName();
        } else {
          startExchange();
          requestUpdate();
        }
      }
    }
    return;
  }

  if (state == EXCHANGING) {
    unsigned long now = millis();

    // Poll scan results every second
    if (now - lastScanMs >= SCAN_INTERVAL_MS) {
      lastScanMs = now;
      pollScanResults();
    }

    // Timeout or Back → end exchange
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        (now - exchangeStart >= EXCHANGE_DURATION_MS)) {
      stopExchange();
      if (!received.empty()) {
        contactIndex = 0;
        state = RECEIVED;
      } else {
        state = IDLE;
      }
      requestUpdate();
    }
    return;
  }

  if (state == RECEIVED) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = IDLE;
      idleMenuIndex = 0;
      requestUpdate();
      return;
    }

    buttonNavigator.onNext([this] {
      contactIndex = ButtonNavigator::nextIndex(contactIndex,
                                                static_cast<int>(received.size()));
      requestUpdate();
    });
    buttonNavigator.onPrevious([this] {
      contactIndex = ButtonNavigator::previousIndex(contactIndex,
                                                    static_cast<int>(received.size()));
      requestUpdate();
    });

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (!received.empty()) {
        saveContact(received[contactIndex]);
        // Remove saved contact from list so user can save others
        received.erase(received.begin() + contactIndex);
        if (received.empty()) {
          state = IDLE;
          idleMenuIndex = 0;
        } else {
          contactIndex = 0;
        }
        requestUpdate();
      }
    }
    return;
  }
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void BleContactExchangeActivity::render(RenderLock&&) {
  renderer.clearScreen();
  switch (state) {
    case IDLE:       renderIdle();       break;
    case EXCHANGING: renderExchanging(); break;
    case RECEIVED:   renderReceived();   break;
  }
  renderer.displayBuffer();
}

void BleContactExchangeActivity::renderIdle() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth  = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer,
                 Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 "Contact Exchange");

  const int contentTop = metrics.topPadding + metrics.headerHeight + 12;

  // Show current contact info
  char buf[48];
  int y = contentTop;

  renderer.drawText(SMALL_FONT_ID, 16, y, "My Info:");
  y += renderer.getLineHeight(SMALL_FONT_ID) + 4;

  if (myName[0] != '\0') {
    snprintf(buf, sizeof(buf), "Name:  %s", myName);
  } else {
    snprintf(buf, sizeof(buf), "Name:  (not set)");
  }
  renderer.drawText(UI_10_FONT_ID, 16, y, buf);
  y += renderer.getLineHeight(UI_10_FONT_ID) + 4;

  if (myPhone[0] != '\0') {
    snprintf(buf, sizeof(buf), "Phone: %s", myPhone);
  } else {
    snprintf(buf, sizeof(buf), "Phone: (not set)");
  }
  renderer.drawText(UI_10_FONT_ID, 16, y, buf);
  y += renderer.getLineHeight(UI_10_FONT_ID) + 20;

  // 2-item menu
  const int menuCount = 2;
  const char* labels[2] = {"Edit My Info", "Start Exchange"};

  GUI.drawList(renderer,
               Rect{0, y, pageWidth, pageHeight - y - metrics.buttonHintsHeight - 8},
               menuCount, idleMenuIndex,
               [&labels](int i) -> std::string { return labels[i]; });

  const auto hints = mappedInput.mapLabels("Back", "Select", "^", "v");
  GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4);
}

void BleContactExchangeActivity::renderExchanging() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth  = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer,
                 Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 "Exchanging...");

  const int contentTop = metrics.topPadding + metrics.headerHeight + 20;

  // Progress bar (time-based)
  unsigned long elapsed = millis() - exchangeStart;
  if (elapsed > EXCHANGE_DURATION_MS) elapsed = EXCHANGE_DURATION_MS;
  int barW = pageWidth - 32;
  int filled = static_cast<int>((long)barW * (long)elapsed / (long)EXCHANGE_DURATION_MS);
  renderer.drawRect(16, contentTop, barW, 12, true);
  if (filled > 0) renderer.fillRect(16, contentTop, filled, 12, true);

  int y = contentTop + 24;

  // Countdown
  unsigned long remaining = (EXCHANGE_DURATION_MS - elapsed) / 1000 + 1;
  char countBuf[24];
  snprintf(countBuf, sizeof(countBuf), "Scanning... %lus", remaining);
  renderer.drawCenteredText(UI_10_FONT_ID, y, countBuf);
  y += renderer.getLineHeight(UI_10_FONT_ID) + 16;

  // Received count
  char foundBuf[32];
  snprintf(foundBuf, sizeof(foundBuf), "Contacts found: %d", static_cast<int>(received.size()));
  renderer.drawCenteredText(UI_10_FONT_ID, y, foundBuf);
  y += renderer.getLineHeight(UI_10_FONT_ID) + 24;

  // Live list of found contacts (up to 4)
  const int showMax = 4;
  int shown = static_cast<int>(received.size());
  if (shown > showMax) shown = showMax;
  for (int i = 0; i < shown; i++) {
    char cbuf[48];
    snprintf(cbuf, sizeof(cbuf), "+ %s", received[i].name);
    renderer.drawCenteredText(SMALL_FONT_ID, y, cbuf);
    y += renderer.getLineHeight(SMALL_FONT_ID) + 4;
  }

  const auto hints = mappedInput.mapLabels("Stop", "", "", "");
  GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4);
}

void BleContactExchangeActivity::renderReceived() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth  = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer,
                 Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 "Received Contacts");

  const int listTop    = metrics.topPadding + metrics.headerHeight + 4;
  const int listBottom = pageHeight - metrics.buttonHintsHeight - 8;

  const int count = static_cast<int>(received.size());
  GUI.drawList(renderer,
               Rect{0, listTop, pageWidth, listBottom - listTop},
               count, contactIndex,
               [this](int i) -> std::string { return received[i].name; },
               [this](int i) -> std::string { return received[i].phone; });

  const auto hints = mappedInput.mapLabels("Back", "Save", "^", "v");
  GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4);
}
