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

  // Returns true if a .syn file exists for the active dictionary.
  // Gates all synonym UI — checked at runtime against the physical file.
  static bool hasSyn();

  // Set the active dictionary base path (e.g. "/dictionary/dict-en-en/dict-data").
  static void setActivePath(const char* folderPath);

  // Returns the active base path (empty string if none configured).
  static const char* getActivePath();

  // Validates the stored dictionary path in SETTINGS against the SD card.
  // If the path is missing or the required files are gone, resets SETTINGS.dictionaryPath
  // to empty, clears the active path, and saves settings. Returns true if valid.
  static bool isValidDictionary();

  // Parse the .ifo file in folderPath and return metadata.
  // Also checks for .syn and .dict.dz presence.
  static DictInfo readInfo(const char* folderPath);

  // Look up word in .idx (via .idx.oft if present). Returns definition or empty string.
  static std::string lookup(const std::string& word,
                            const std::function<void(int percent)>& onProgress = nullptr,
                            const std::function<bool()>& shouldCancel = nullptr);

  // Look up word in .syn (via .syn.oft if present).
  // Returns the canonical headword from .idx, or empty string if not found.
  static std::string lookupSynonym(const std::string& word);

  static std::string cleanWord(const std::string& word);
  static std::vector<std::string> getStemVariants(const std::string& word);

  // Returns up to maxResults words from .idx that are close in edit distance to word.
  // Requires .idx to be accessible; uses .idx.oft if present for neighbourhood search.
  static std::vector<std::string> findSimilar(const std::string& word, int maxResults);

 private:
  static char activeFolderPath[500];

  // Build full file paths from the active base path.
  static void buildPath(char* buf, size_t len, const char* ext);

  // Read a null-terminated word from an open file into buf (max bufSize-1 chars).
  // Returns the number of characters read (excluding null), or -1 on error.
  static int readWordInto(FsFile& file, char* buf, size_t bufSize);

  // Read the word at ordinal `ordinal` in .idx using .idx.oft (if present).
  // Returns the word string, or empty if not found.
  static std::string wordAtOrdinal(uint32_t ordinal);

  static std::string readDefinition(uint32_t offset, uint32_t size);

  // Binary search .oft to find the page boundary bytes in src containing target.
  // On return, *startByte and *endByte delimit the 32-word page to scan linearly.
  // srcFileSize is used as the upper bound when the page is the last one.
  static void findPageBounds(FsFile& oft, FsFile& src, uint32_t srcFileSize,
                             const char* target, uint32_t* startByte, uint32_t* endByte);

  static int editDistance(const std::string& a, const std::string& b, int maxDist);
};
