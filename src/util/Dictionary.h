#pragma once
#include <functional>
#include <string>

class Dictionary {
 public:
  static bool exists();
  static std::string lookup(const std::string& word,
                            const std::function<void(int percent)>& onProgress = nullptr);
  static std::string cleanWord(const std::string& word);
};
