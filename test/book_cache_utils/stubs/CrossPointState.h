#pragma once

#include <string>

class CrossPointState {
 public:
  std::string openEpubPath;

  static CrossPointState& getInstance() {
    static CrossPointState state;
    return state;
  }

  bool saveToFile() const { return true; }
};

#define APP_STATE CrossPointState::getInstance()
