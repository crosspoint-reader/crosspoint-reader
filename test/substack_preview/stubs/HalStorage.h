#pragma once

// Host-build stand-in for lib/hal/HalStorage.h. Bitmap.h/.cpp (pulled in by
// GfxRenderer's drawBitmap/drawBitmap1Bit) need a HalFile type to reference,
// but substack_preview never actually loads a cover image, so these bodies
// are never exercised — they only need to exist for the linker.
#include <cstddef>
#include <cstdint>

class HalFile {
 public:
  explicit operator bool() const { return false; }
  bool seek(uint32_t) { return false; }
  void seekCur(int32_t) {}
  int read() { return -1; }
  size_t read(void*, size_t) { return 0; }
};
