#pragma once
#include <cstdint>
#include <string>

class CrossPointState {
  // Static instance
  static CrossPointState instance;

 public:
  static constexpr uint8_t SLEEP_RECENT_COUNT = 16;

  std::string openEpubPath;
  uint16_t recentSleepImages[SLEEP_RECENT_COUNT] = {};  // circular buffer of recent wallpaper indices
  uint8_t recentSleepPos = 0;                           // next write slot
  uint8_t recentSleepFill = 0;                          // valid entries (0..SLEEP_RECENT_COUNT)
  uint8_t readerActivityLoadCount = 0;
  bool lastSleepFromReader = false;
  bool showBootScreen = true;
  // Runtime-only (never persisted): set when the reader exits a book with BLE Sync
  // on, consumed by the main loop to launch a book-exit sync. Zero cost when off.
  bool blePendingSync = false;
  // Runtime-only: book path whose progress.bin a BLE reconcile just overwrote
  // with the phone's newer position. Consumed by EpubReaderActivity::loop() to
  // jump the open reader there — otherwise the stale in-RAM position survives
  // and the exit save clobbers the applied one with a fresh timestamp.
  std::string bleAppliedPath;

  // Returns true if idx was shown within the last checkCount picks.
  // Walks backwards from the most recently written slot.
  bool isRecentSleep(uint16_t idx, uint8_t checkCount) const;

  void pushRecentSleep(uint16_t idx);
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
