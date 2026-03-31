#pragma once
#include <HalStorage.h>

#include <cstdint>
#include <string>
#include <vector>

// Plain-function-pointer callbacks for Dictionary::lookup.
// Zero overhead: no heap allocation, no vtable, no std::function bloat.
struct DictLookupCallbacks {
  void* ctx = nullptr;
  void (*onProgress)(void* ctx, int percent) = nullptr;
  bool (*shouldCancel)(void* ctx) = nullptr;
};

// Metadata parsed from a StarDict .ifo file.
struct DictInfo {
  char bookname[128] = "";
  char website[128] = "";
  char date[32] = "";
  char description[256] = "";
  char sametypesequence[16] = "";
  uint32_t wordcount = 0;
  uint32_t altFormCount = 0;
  uint32_t idxfilesize = 0;
  bool hasAltForms = false;
  bool isCompressed = false;  // .dict.dz present but no .dict
  char lang[32] = "";         // e.g. "en-en", "el-el"
  bool valid = false;
};

class Dictionary {
 public:
  // Returns true if a dictionary is configured and all required files exist.
  static bool exists();

  // Returns true if a .syn file exists for the active dictionary.
  // Gates all alternate-form UI — checked at runtime against the physical file.
  static bool hasAltForms();

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
  static std::string lookup(const std::string& word, const DictLookupCallbacks& cbs = {});

  // Look up word in .syn (via .syn.oft if present).
  // Returns the canonical headword from .idx, or empty string if not found.
  static std::string resolveAltForm(const std::string& word);

  static std::string cleanWord(const std::string& word);
  static std::vector<std::string> getStemVariants(const std::string& word);

  // Returns up to maxResults words from .idx that are close in edit distance to word.
  // Requires .idx to be accessible; uses .idx.oft if present for neighbourhood search.
  static std::vector<std::string> findSimilar(const std::string& word, int maxResults);

 private:
  static char activeFolderPath[500];
  // Shared path construction buffer. Dictionary functions are always called
  // sequentially; this avoids putting a 520-byte array on the stack in every caller.
  static char pathBuf[520];

  // Shared word read buffer. Lookup functions are single-threaded; this avoids
  // putting a 256-byte array on the stack in every caller (and 512B peak when nested).
  static char wordBuf[256];

  // Build full file paths from the active base path.
  static void buildPath(const char* ext);

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
  static void findPageBounds(FsFile& oft, FsFile& src, uint32_t srcFileSize, const char* target, uint32_t* startByte,
                             uint32_t* endByte);

  static int editDistance(const std::string& a, const std::string& b, int maxDist);
};
