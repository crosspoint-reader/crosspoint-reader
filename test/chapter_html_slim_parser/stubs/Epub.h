#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class Epub {
 public:
  static inline bool enableGifProbeForTest = false;
  std::vector<uint8_t> imageBytes;
  mutable size_t headerProbes = 0;
  mutable size_t fullExtractions = 0;
  template <typename Output>
  bool readItemContentsToStream(const std::string&, Output& output, size_t, bool allowEarlyStop = false) const {
    if (imageBytes.empty()) return false;
    if (allowEarlyStop)
      ++headerProbes;
    else
      ++fullExtractions;
    const size_t written = output.write(imageBytes.data(), imageBytes.size());
    return written == imageBytes.size() || allowEarlyStop;
  }
};
