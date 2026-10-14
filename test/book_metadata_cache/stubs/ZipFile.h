#pragma once
#include <cstdint>
#include <deque>
#include <string>
class ZipFile {
 public:
  struct SizeTarget {
    uint64_t hash;
    uint16_t len;
    uint16_t index;
  };
  explicit ZipFile(const std::string&) {}
  bool open() { return false; }
  void close() {}
  static uint64_t fnvHash64(const char*, size_t) { return 0; }
  int fillUncompressedSizes(const std::deque<SizeTarget>&, std::deque<uint32_t>&) { return 0; }
  bool getInflatedFileSize(const char*, size_t*) { return false; }
};
