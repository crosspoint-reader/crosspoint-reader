#pragma once

#include <array>
#include <cstdint>
#include <string>

class BookFinishedCache {
 public:
  static BookFinishedCache& getInstance();

  bool tryGet(const std::string& bookPath, bool& finished);
  bool resolve(const std::string& bookPath, bool& finished);
  void put(const std::string& bookPath, bool finished);
  void clear();
  bool saveIfDirty();

 private:
  struct Entry {
    uint32_t key = 0;
    int8_t value = 0;
    uint32_t age = 0;
    bool used = false;
  };

  static constexpr size_t MAX_ENTRIES = 64;
  static constexpr char CACHE_PATH[] = "/.crosspoint/book_finished_cache.bin";
  static constexpr uint32_t CACHE_MAGIC = 0x46434642;  // "BFCF"
  static constexpr uint8_t CACHE_VERSION = 1;

  std::array<Entry, MAX_ENTRIES> entries = {};
  uint32_t ageCounter = 0;
  bool loaded = false;
  bool dirty = false;

  static uint32_t makeKey(const std::string& bookPath);
  void ensureLoaded();
  bool loadFromDisk();
  bool saveToDisk();
  int findIndex(uint32_t key) const;
  int findEvictionIndex() const;
  static bool loadFinishedFromStats(const std::string& bookPath, bool& finished);
};

#define BOOK_FINISHED_CACHE BookFinishedCache::getInstance()