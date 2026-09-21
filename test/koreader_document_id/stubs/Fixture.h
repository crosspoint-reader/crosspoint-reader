#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace documentIdFixture {

struct State {
  size_t fileSize = 0;
  bool openSucceeds = true;
  std::optional<size_t> failedSeek;
  std::optional<size_t> faultReadOffset;
  int faultReadResult = 0;
  int closedFiles = 0;
  std::vector<size_t> seeks;
  std::vector<std::pair<size_t, size_t>> reads;
  std::vector<size_t> hashFeedSizes;
  bool oversizedHashFeed = false;
};

inline State state;

inline uint8_t patternByte(const size_t offset) {
  return static_cast<uint8_t>((static_cast<uint64_t>(offset) * 37 + offset / 251) % 256);
}

}  // namespace documentIdFixture
