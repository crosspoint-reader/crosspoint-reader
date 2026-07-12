#pragma once
#include <cstring>
#include <string>

class DuressManager {
 public:
  static DuressManager& getInstance() {
    static DuressManager instance;
    return instance;
  }

  void activate() { duressActive = true; }
  void deactivate() { duressActive = false; }
  bool isActive() const { return duressActive; }

  // If duress active, redirect /biscuit/ paths to /biscuit_safe/
  std::string resolvePath(const char* path) const {
    if (duressActive && strncmp(path, "/biscuit/", 9) == 0) {
      return std::string("/biscuit_safe/") + (path + 9);
    }
    return path;
  }

 private:
  DuressManager() = default;
  bool duressActive = false;
};

#define DURESS DuressManager::getInstance()
