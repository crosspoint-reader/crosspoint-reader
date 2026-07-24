#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class DictionaryHistoryStore {
 public:
  static constexpr size_t MAX_ENTRIES = 15;
  static constexpr size_t MAX_FILE_BYTES = 4096;
  static constexpr size_t MAX_QUERY_BYTES = 255;

  static DictionaryHistoryStore& getInstance();

  bool load();
  void record(const std::string& query);
  bool flush();
  bool clear();
  const std::vector<std::string>& entries();
  bool isWritable() const { return writable_; }
#ifdef UNIT_TEST
  void resetForTests() {
    loaded_ = false;
    writable_ = true;
    dirty_ = false;
    entries_.clear();
  }
#endif

 private:
  static bool validate(const uint8_t* data, size_t size, void* context);
  bool parse(const std::string& data);
  std::string serialize() const;

  bool loaded_ = false;
  bool writable_ = true;
  bool dirty_ = false;
  std::vector<std::string> entries_;
};

#define DICTIONARY_HISTORY DictionaryHistoryStore::getInstance()
