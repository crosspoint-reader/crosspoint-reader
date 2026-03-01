#pragma once
#include <iosfwd>
#include <string>

class CrossPointState {
  // Static instance
  static CrossPointState instance;

 public:
  std::string openEpubPath;
  uint8_t lastSleepImage = 0;
  uint8_t readerActivityLoadCount = 0;
  bool lastSleepFromReader = false;
  // Exponential backoff for WiFi auto-connect.
  // skipCount: remaining wakes to skip before next attempt (decremented each wake).
  // backoffLevel: exponent for next skip window; skip = (1 << backoffLevel) - 1.
  // Reset both to 0 when a successful API call is observed.
  uint8_t wifiAutoConnectSkipCount = 0;
  uint8_t wifiAutoConnectBackoffLevel = 0;
  ~CrossPointState() = default;

  // Get singleton instance
  static CrossPointState& getInstance() { return instance; }

  bool saveToFile() const;

  bool loadFromFile();

 private:
  bool loadFromBinaryFile();
};

// Helper macro to access settings
#define APP_STATE CrossPointState::getInstance()
