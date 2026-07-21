#pragma once

#include <cstddef>
#include <cstdint>

class InflateStream {
 public:
  enum class Status { Ok, Done, Error };
  using FillFn = size_t (*)(void*, const uint8_t**);
  bool init(bool) { return false; }
  void setFill(FillFn, void*) {}
  bool read(uint8_t*, size_t) { return false; }
  Status readAtMost(uint8_t*, size_t, size_t* produced) {
    *produced = 0;
    return Status::Error;
  }
};
