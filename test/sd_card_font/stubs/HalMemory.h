#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
inline bool sdFontFailPsram = false;
class HalMemory {
 public:
  using PsramBuffer = std::unique_ptr<uint8_t[]>;
  static PsramBuffer allocatePsram(size_t bytes) {
    return sdFontFailPsram ? nullptr : PsramBuffer(new (std::nothrow) uint8_t[bytes]);
  }
};
