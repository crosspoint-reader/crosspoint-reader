#pragma once
#include <HalStorage.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Metadata parsed from a StarDict .ifo file.
struct DictInfo {
  char bookname[128] = "";
  char website[128] = "";
  char date[32] = "";
  char description[256] = "";
  char sametypesequence[16] = "";
  uint32_t wordcount = 0;
  uint32_t synwordcount = 0;
  uint32_t idxfilesize = 0;
  bool hasSyn = false;
  bool isCompressed = false;  // .dict.dz present but no .dict
  bool valid = false;
};

class Dictionary {
 public:
  // Returns true if a dictionary is configured and all required files exist.
  static bool exists();

  // Set the active dictionary folder path (e.g. "/dictionary/dict-en-en").
  // Resets the loaded index so the next lookup re-loads from the new path.
  static void setActivePath(const char* folderPath);

  // Returns the active folder path (empty string if none configured).
  static const char* getActivePath();

  // Returns true if the given folder contains a valid, usable StarDict dictionary
  // (.ifo + .idx + .dict all present). Used by the dictionary picker to scan.
  static bool isValidDictionary(const char* folderPath);

  // Parse the .ifo file in folderPath and return metadata.
  // Also checks for .syn and .dict.dz presence.
  static DictInfo readInfo(const char* folderPath);

  static std::string lookup(const std::string& word,
                            const std::function<void(int percent)>& onProgress = nullptr,
                            const std::function<bool()>& shouldCancel = nullptr);
  static std::string cleanWord(const std::string& word);
  static std::vector<std::string> getStemVariants(const std::string& word);
  static std::vector<std::string> findSimilar(const std::string& word, int maxResults);

 private:
  static constexpr int SPARSE_INTERVAL = 512;

  static char activeFolderPath[500];
  static std::vector<uint32_t> sparseOffsets;
  static uint32_t totalWords;
  static bool indexLoaded;

  // Build full file paths from the active folder path.
  static void buildPath(char* buf, size_t len, const char* filename);

  static bool loadIndex(const std::function<void(int percent)>& onProgress,
                        const std::function<bool()>& shouldCancel);
  static std::string readWord(FsFile& file);
  static std::string readDefinition(uint32_t offset, uint32_t size);
  static int editDistance(const std::string& a, const std::string& b, int maxDist);
};
