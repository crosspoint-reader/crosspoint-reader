#pragma once

#include <cstdint>
#include <string>

class HalFile;

namespace LegacyStateCodec {

constexpr uint8_t CURRENT_VERSION = 4;
constexpr uint32_t MAX_BOOK_PATH_BYTES = 4096;

enum class DecodeStatus : uint8_t { Ok, FutureVersion, Invalid, IoError };

struct State {
  std::string openBookPath;
  uint8_t lastSleepImage = UINT8_MAX;
  uint8_t readerActivityLoadCount = 0;
  bool lastSleepFromReader = false;
};

DecodeStatus decode(HalFile& file, State& state);

}  // namespace LegacyStateCodec
