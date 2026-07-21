#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <string>

class BoundedFileReader {
 public:
  BoundedFileReader(HalFile& file, const uint64_t begin, const uint64_t end)
      : file_(file),
        position_(begin),
        end_(end),
        valid_(begin <= end && end <= file.fileSize64() && file.seek64(begin)) {}

  template <typename T>
  bool readPod(T& value) {
    value = {};
    return readBytes(&value, sizeof(T));
  }

  bool readBool(bool& value) {
    uint8_t serialized = 0;
    if (!readPod(serialized) || serialized > 1) {
      value = false;
      return false;
    }
    value = serialized != 0;
    return true;
  }

  bool readBytes(void* destination, const size_t length) {
    if (!valid_ || (!destination && length != 0) || length > remaining() ||
        (length != 0 && file_.read(destination, length) != static_cast<int>(length))) {
      valid_ = false;
      return false;
    }
    position_ += length;
    return true;
  }

  bool readString(std::string& value, const uint32_t maximumBytes, const bool allowEmpty = true) {
    uint32_t length = 0;
    value.clear();
    if (!readPod(length) || length > maximumBytes || (!allowEmpty && length == 0) || length > remaining()) {
      valid_ = false;
      return false;
    }
    value.resize(length);
    return length == 0 || readBytes(value.data(), length);
  }

  bool skip(const uint64_t length) {
    if (!valid_ || length > remaining() || !file_.seek64(position_ + length)) {
      valid_ = false;
      return false;
    }
    position_ += length;
    return true;
  }

  uint64_t position() const { return position_; }
  uint64_t remaining() const { return valid_ ? end_ - position_ : 0; }
  bool atEnd() const { return valid_ && position_ == end_; }
  bool valid() const { return valid_; }

 private:
  HalFile& file_;
  uint64_t position_ = 0;
  uint64_t end_ = 0;
  bool valid_ = false;
};
