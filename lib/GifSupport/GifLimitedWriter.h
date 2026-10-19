#pragma once

#include <Print.h>

#include <cstddef>
#include <cstdint>

// A short write stops ZipFile streaming even when the ZIP directory lies about size.
class GifLimitedWriter final : public Print {
  Print& output;
  size_t remaining;

 public:
  GifLimitedWriter(Print& output, size_t limit) : output(output), remaining(limit) {}
  size_t write(uint8_t value) override { return write(&value, 1); }
  size_t write(const uint8_t* data, size_t size) override {
    if (size > remaining) return 0;
    const size_t written = output.write(data, size);
    remaining -= written;
    return written;
  }
};
