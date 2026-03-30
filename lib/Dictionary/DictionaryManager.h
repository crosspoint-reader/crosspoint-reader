#pragma once
#include <HalStorage.h>

#include "DictTypes.h"
#include "DictionaryIndex.h"

class DictionaryManager {
 public:
  static constexpr int MAX_DICTIONARIES = 16;
  static constexpr int MAX_RESULTS = 8;
  static constexpr const char* DICT_DIR = "/.crosspoint/dictionaries";
  static constexpr const char* ENABLED_FILE = "/.crosspoint/dictionaries/enabled.json";

  void scan();
  void rescan() {
    scanned = false;
    scan();
  }
  int lookup(const char* word, DictResult* results, int maxResults);
  bool hasEnabledDictionaries() const;

  int getDictionaryCount() const { return dictCount; }
  const DictFileInfo& getDictionary(int index) const { return dictionaries[index]; }
  void setEnabled(int index, bool enabled);
  void saveEnabledState();

 private:
  DictFileInfo dictionaries[MAX_DICTIONARIES] = {};
  int dictCount = 0;
  DictionaryIndex index;
  bool scanned = false;

  void loadEnabledState();

 public:
  static bool normalizeWord(const char* word, char* outBuf, int outSize);
};
