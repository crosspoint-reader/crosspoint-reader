#pragma once

#include "DictTypes.h"

class LookupHistory {
 public:
  static constexpr int MAX_ENTRIES = 10;
  static constexpr const char* HISTORY_FILE = "/.dictionaries/history.txt";

  /// Load history from SD card file. Safe to call multiple times (reloads).
  void load();

  /// Save history to SD card file. Only writes if dirty flag is set.
  void save();

  /// Add a word to history. Normalizes, deduplicates (bumps to top), evicts oldest if full.
  void addWord(const char* word);

  /// Remove entry at index, shifting remaining entries up.
  void removeWord(int index);

  /// Remove all entries.
  void clear();

  const char* getWord(int index) const;
  int getCount() const { return count; }

 private:
  char words[MAX_ENTRIES][DICT_WORD_MAX] = {};
  int count = 0;
  bool dirty = false;

  /// Shift entries at [0..index-1] down by one, opening slot 0.
  void shiftDownAndInsertFront(int index, const char* word);
};
