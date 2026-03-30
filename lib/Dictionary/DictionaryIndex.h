#pragma once
#include <HalStorage.h>

#include "DictTypes.h"

class DictionaryIndex {
 public:
  bool ensureIndex(const char* dictPath, bool& outCorrupt, bool& outReadOnly);
  bool lookup(const char* dictPath, const char* word, char* outDef, int outDefSize);
  bool linearScan(const char* dictPath, const char* word, char* outDef, int outDefSize);

 private:
  bool generateIndex(const char* dictPath, const char* idxPath, bool& outCorrupt);
  bool validateIndex(const char* dictPath, const char* idxPath);
  uint32_t computeSpotCheckHash(HalFile& file, uint32_t fileSize);
  int32_t binarySearchIndex(HalFile& idxFile, uint32_t entryCount, const char* word);
};
