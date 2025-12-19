#pragma once
#include <cstdint>
#include <iosfwd>

class CrossPointSettings {
 private:
  // Private constructor for singleton
  CrossPointSettings() = default;

  // Static instance
  static CrossPointSettings instance;

 public:
  // Delete copy constructor and assignment
  CrossPointSettings(const CrossPointSettings&) = delete;
  CrossPointSettings& operator=(const CrossPointSettings&) = delete;

  // Sleep screen settings
  uint8_t whiteSleepScreen = 0;

  // Text rendering settings
  uint8_t extraParagraphSpacing = 1;
  uint8_t hyphenationEnabled = 1;

  ~CrossPointSettings() = default;

  // Get singleton instance
  static CrossPointSettings& getInstance() { return instance; }

  bool saveToFile() const;
  bool loadFromFile();
};

// Helper macro to access settings
#define SETTINGS CrossPointSettings::getInstance()
