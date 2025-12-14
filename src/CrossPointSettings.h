#pragma once
#include <iosfwd>

class CrossPointSettings {
 public:
  // Sleep screen settings
  bool whiteSleepScreen = false;
  
  ~CrossPointSettings() = default;

  bool saveToFile() const;
  bool loadFromFile();
};