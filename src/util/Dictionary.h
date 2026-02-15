#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class FsFile;

class Dictionary {
 public:
  static bool exists();
  static std::string lookup(const std::string& word, const std::function<void(int percent)>& onProgress = nullptr,
                            const std::function<bool()>& shouldCancel = nullptr);
  static std::string cleanWord(const std::string& word);
  static std::vector<std::string> getStemVariants(const std::string& word);
  static std::vector<std::string> findSimilar(const std::string& word, int maxResults);

 private:
  static constexpr int SPARSE_INTERVAL = 512;

  static std::vector<uint32_t> sparseOffsets;
  static uint32_t totalWords;
  static bool indexLoaded;

  static bool loadIndex(const std::function<void(int percent)>& onProgress, const std::function<bool()>& shouldCancel);
  static bool loadCachedIndex();
  static void saveCachedIndex();
  static std::string readWord(FsFile& file);
  static std::string readDefinition(uint32_t offset, uint32_t size);
  static int editDistance(const std::string& a, const std::string& b, int maxDist);
};
