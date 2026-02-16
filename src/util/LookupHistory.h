#pragma once
#include <string>
#include <vector>

class LookupHistory {
 public:
  static std::vector<std::string> load(const std::string& cachePath);
  static void addWord(const std::string& cachePath, const std::string& word);
  static void removeWord(const std::string& cachePath, const std::string& word);
  static bool hasHistory(const std::string& cachePath);

 private:
  static std::string filePath(const std::string& cachePath);
  static constexpr int MAX_ENTRIES = 500;
};
