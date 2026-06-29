#pragma once
#include <HalStorage.h>

#include <cstdint>
#include <string>
#include <vector>

// Helper for constructing dictionary file paths from a folder base path.
struct DictPaths {
  const std::string& folder;
  explicit DictPaths(const std::string& f) : folder(f) {}

  std::string idx() const { return folder + ".idx"; }
  std::string dict() const { return folder + ".dict"; }
  std::string ifo() const { return folder + ".ifo"; }
  std::string dictDz() const { return folder + ".dict.dz"; }
};

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
  uint32_t idxfilesize = 0;
  bool isCompressed = false;  // .dict.dz present but no .dict
  char lang[32] = "";         // e.g. "en-en", "el-el"
  bool valid = false;
};

// Result of an index search — file location of a definition without reading it.
struct DictLocation {
  std::string folderPath;  // dictionary base path (e.g. /dictionary/dict-en-en/dict-data)
  uint32_t offset = 0;     // byte offset in .dict file
  uint32_t size = 0;       // byte length in .dict file
  bool found = false;
};

class Dictionary {
 public:
  static constexpr unsigned long LONG_PRESS_MS = 600;

  // Returns the active dictionary folder base path by reading dictionary.bin from the SD card.
  // If cachePath is non-null and non-empty, reads <cachePath>/dictionary.bin (per-book override).
  // Otherwise reads /.crosspoint/dictionary.bin (global setting).
  // Returns empty string if no dictionary is configured or the file cannot be read.
  static std::string readDictPath(const char* cachePath = nullptr);

  // Writes folderPath to /.crosspoint/dictionary.bin (global setting).
  // Pass empty string to clear the global dictionary.
  static void saveGlobalDictPath(const char* folderPath);

  // Returns true if a dictionary is configured and all required files exist.
  static bool exists(const char* cachePath = nullptr);

  // Validates the dictionary path stored in /.crosspoint/dictionary.bin against the SD card.
  // If the path is missing or the required files are gone, clears the file. Returns true if valid.
  static bool isValidDictionary();

  // Parse the .ifo file in folderPath and return metadata.
  // Also checks for .syn and .dict.dz presence.
  static DictInfo readInfo(const char* folderPath);

  // Search .idx for word. Returns file location without reading content.
  static DictLocation locate(const std::string& word, const DictLookupCallbacks& cbs = {},
                             const char* cachePath = nullptr);

  // Look up word in .idx. Returns definition or empty string.
  static std::string lookup(const std::string& word, const DictLookupCallbacks& cbs = {},
                            const char* cachePath = nullptr);

  static std::string cleanWord(const std::string& word);
  static std::vector<std::string> getStemVariants(const std::string& word);

  // Returns up to maxResults words from .idx that are close in edit distance to word.
  // Requires .idx to be accessible; the minimal engine scans the full index.
  static std::vector<std::string> findSimilar(const std::string& word, int maxResults, const char* cachePath = nullptr);

 private:
  // Shared word read buffer. Lookup functions are single-threaded; this avoids
  // putting a 256-byte array on the stack in every caller (and 512B peak when nested).
  static char wordBuf[256];

  // Read a null-terminated word from an open file into buf (max bufSize-1 chars).
  // Returns the number of characters read (excluding null), or -1 on error.
  static int readWordInto(HalFile& file, char* buf, size_t bufSize);

  static std::string readDefinition(const std::string& folderPath, uint32_t offset, uint32_t size);

  static int editDistance(const std::string& a, const std::string& b, int maxDist);
};
