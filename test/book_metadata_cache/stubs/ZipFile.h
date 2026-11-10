#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>

// Inflated sizes of the archive's entries, by path. The batch lookup finds nothing, so every size
// is looked up by path.
inline std::map<std::string, uint32_t> zipEntrySizes;
inline unsigned zipOpenCalls = 0;

class ZipFile {
 public:
  struct SizeTarget {
    uint64_t hash;
    uint16_t len;
    uint16_t index;
  };

  static uint64_t fnvHash64(const char*, size_t) { return 0; }
  explicit ZipFile(const std::string&) {}
  bool open() {
    ++zipOpenCalls;
    return true;
  }
  bool close() { return true; }
  int fillUncompressedSizes(std::deque<SizeTarget>&, std::deque<uint32_t>&) { return 0; }

  bool getInflatedFileSize(const char* path, size_t* size) const {
    const auto it = zipEntrySizes.find(path);
    if (it == zipEntrySizes.end()) return false;
    *size = it->second;
    return true;
  }
};
