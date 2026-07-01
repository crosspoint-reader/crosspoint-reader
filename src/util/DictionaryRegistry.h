#pragma once

#include <atomic>
#include <string>
#include <vector>

// One installed StarDict dictionary discovered on the SD card.
struct DictionaryEntry {
  std::string name;      // folder name, e.g. "dict-en-en"
  std::string stem;      // file stem (".idx"/".ifo" base), e.g. "dict-data"
  std::string basePath;  // <root>/<name>/<stem>, e.g. "/dictionaries/dict-en-en/dict-data"
};

// Discovers installed dictionaries on the SD card. Mirrors SdCardFontRegistry:
// discover() scans the card and populates entries_; the settings UI enumerates
// them on both device and web. The *active* selection is not stored here — it
// lives in dictionary.bin (see Dictionary::readDictPath/saveGlobalDictPath).
class DictionaryRegistry {
 public:
  // Scan the SD card, populate entries_ (sorted by folder name). Returns true if any found.
  bool discover();

  const std::vector<DictionaryEntry>& getEntries() const { return entries_; }
  int count() const { return static_cast<int>(entries_.size()); }
  const std::string& root() const { return root_; }

  // Index of the entry whose basePath == path, or -1 if not found / path empty.
  int indexOf(const std::string& basePath) const;

  // Mark the registry as needing a re-scan. Thread-safe (callable from the web task).
  void markDirty() { dirty_.store(true, std::memory_order_release); }

  // Re-scan if marked dirty, then clear the flag. Mirrors SdCardFontSystem::refreshIfDirty().
  void refreshIfDirty() {
    if (dirty_.exchange(false, std::memory_order_acquire)) discover();
  }

 private:
  std::vector<DictionaryEntry> entries_;  // sorted alphabetically by name
  std::string root_;                      // active dictionary root dir on the SD card
  std::atomic<bool> dirty_{false};
};

// Global dictionary registry instance (defined in main.cpp).
extern DictionaryRegistry dictionaryRegistry;
