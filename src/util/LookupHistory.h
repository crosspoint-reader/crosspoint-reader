#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Per-book lookup history. Stored as <cachePath>/dictionary_history.txt.
// Oldest entry at top; newest at bottom. No deduplication.
class LookupHistory {
 public:
  enum class Status { Direct = 'D', Stem = 'T', AltForm = 'Y', Suggestion = 'S', NotFound = 'X' };

  struct Entry {
    std::string word;
    std::string headword;
    Status status = Status::NotFound;
    uint32_t sourceOffset = 0;
    uint32_t sourceSize = 0;
    bool sourceInTempFile = false;
  };

  // Append word+status. Evicts oldest entries if over cap. Returns new entry count.
  static int addWord(const std::string& cachePath, const std::string& word, Status status,
                     const std::string& headword = "", bool sourceInTempFile = false, uint32_t sourceOffset = 0,
                     uint32_t sourceSize = 0);

  // Conditional addWord: short-circuits if disabled, word empty, or cachePath empty.
  // Single guarded entry point used by all dictionary lookup recording sites.
  static void addWordIf(const std::string& cachePath, const std::string& word, Status status, bool enabled,
                        const std::string& headword = "", bool sourceInTempFile = false, uint32_t sourceOffset = 0,
                        uint32_t sourceSize = 0);

  // Load all entries in most-recent-first order.
  static std::vector<Entry> load(const std::string& cachePath);

  // Get word at 0-based file index (oldest=0). Returns "" if out of range.
  static std::string getWordAt(const std::string& cachePath, int index);

  // Remove entry at 0-based file index. Rewrites file without that entry.
  static bool removeAt(const std::string& cachePath, int index);

 private:
  static std::string filePath(const std::string& cachePath);
  static std::vector<Entry> readAll(const std::string& path);
  static bool writeAll(const std::string& path, const std::vector<Entry>& entries);
};
