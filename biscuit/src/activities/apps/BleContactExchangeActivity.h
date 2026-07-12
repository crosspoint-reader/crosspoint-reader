#pragma once
#include <string>
#include <vector>
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class BleContactExchangeActivity final : public Activity {
 public:
  explicit BleContactExchangeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BleContactExchange", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == EXCHANGING; }

 private:
  enum State { IDLE, EXCHANGING, RECEIVED };
  State state = IDLE;
  ButtonNavigator buttonNavigator;

  // My contact info
  char myName[20] = {};
  char myPhone[16] = {};

  // Received contacts
  struct Contact {
    char name[15];   // 14 chars + null
    char phone[15];  // 14 chars + null
  };
  std::vector<Contact> received;
  int contactIndex = 0;

  // IDLE menu selection (0=Edit Info, 1=Start Exchange)
  int idleMenuIndex = 0;

  // Exchange state
  unsigned long exchangeStart = 0;
  unsigned long lastScanMs = 0;
  static constexpr unsigned long EXCHANGE_DURATION_MS = 10000;
  static constexpr unsigned long SCAN_INTERVAL_MS = 1000;
  bool bleInitialized = false;

  // BLE custom company ID for Biscuit Contact Card
  static constexpr uint16_t BISCUIT_COMPANY_ID = 0xB1CC;

  // Manufacturer data layout:
  // [0-1]   company ID (little-endian) — handled by BLE stack
  // [2-15]  name (14 bytes, null-padded)
  // [16-29] phone (14 bytes, null-padded)
  // Total payload passed to setManufacturerData: 2 + 27 = 29 bytes
  static constexpr int MFG_NAME_OFFSET = 2;
  static constexpr int MFG_NAME_LEN    = 14;
  static constexpr int MFG_PHONE_OFFSET = 16;
  static constexpr int MFG_PHONE_LEN   = 13;
  static constexpr int MFG_TOTAL       = 29;

  static constexpr const char* CONTACTS_PATH   = "/biscuit/contacts.csv";
  static constexpr const char* MY_CONTACT_PATH = "/biscuit/my_contact.dat";

  // Persistent contact data written to / read from flash
  struct MyContactData {
    char name[20];
    char phone[16];
  };

  void loadMyContact();
  void saveMyContact();
  void startExchange();
  void stopExchange();
  void pollScanResults();
  void saveContact(const Contact& contact);
  void editName();
  void editPhone();

  void renderIdle() const;
  void renderExchanging() const;
  void renderReceived() const;
};
