#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace documentIdFixture {

enum class Md5Operation { Context, Initialize, Update, Finalize };

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
  std::optional<Md5Operation> failedMd5Operation;
  std::array<unsigned, 4> md5Calls{};
  unsigned md5ContextsCreated = 0;
  unsigned md5ContextsFreed = 0;
};

inline State state;

inline uint8_t patternByte(const size_t offset) {
  return static_cast<uint8_t>((static_cast<uint64_t>(offset) * 37 + offset / 251) % 256);
}

}  // namespace documentIdFixture
