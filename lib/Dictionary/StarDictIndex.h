#pragma once
#include <HalStorage.h>

#include "DictTypes.h"

class StarDictIndex {
 public:
  // Ensure secondary index (.idx.cp) exists and is valid for the StarDict .idx file.
  // basePath is the dictionary path without extension (e.g., "/.dictionaries/english").
  bool ensureIndex(const char* basePath, bool& outCorrupt, bool& outReadOnly);

  // Look up a word. Reads definition from .dict by offset+size.
  bool lookup(const char* basePath, const char* word, char* outDef, int outDefSize);

  // Parse .ifo file to extract metadata. Returns false if the file is invalid.
  static bool parseIfo(const char* ifoPath, char* outBookname, int nameSize, uint32_t* outWordCount,
                       uint32_t* outIdxFileSize, char* outSameTypeSeq, int seqSize);

 private:
  bool generateIndex(const char* idxPath, const char* cpIdxPath, bool& outCorrupt);
  bool validateIndex(const char* idxPath, const char* cpIdxPath);
  int32_t binarySearchIndex(HalFile& cpIdxFile, uint32_t entryCount, const char* word);
};
