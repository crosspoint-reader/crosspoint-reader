#pragma once
#include <iosfwd>
#include <string>

class CrossPointState {
  // Static instance
  static CrossPointState instance;

 public:
  std::string openEpubPath;
  std::string lastBrowsedFolder = "/";
  unsigned long lastScheduledServerTime = 0;  // Timestamp when scheduled server was last started
  ~CrossPointState() = default;

  // Get singleton instance
  static CrossPointState& getInstance() { return instance; }

  bool saveToFile() const;

  bool loadFromFile();
};

// Helper macro to access settings
#define APP_STATE CrossPointState::getInstance()
