#pragma once
#include <functional>
#include <string>

class FsFile;

class Dictionary {
 public:
  static bool exists();
  static std::string lookup(const std::string& word,
                            const std::function<void(int percent)>& onProgress = nullptr,
                            const std::function<bool()>& shouldCancel = nullptr);
  static std::string cleanWord(const std::string& word);

 private:
  static size_t seekToNearestKey(FsFile& file, size_t pos, size_t fileSize);
  static std::string readKeyAt(FsFile& file, size_t pos);
  static std::string extractDefinition(FsFile& file);
};
