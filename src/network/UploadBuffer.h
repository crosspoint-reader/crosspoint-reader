#pragma once

#include <Memory.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

// What an HTTP upload (/upload, /api/fonts/upload) gathers its chunks in, so the card is written 4 KB at a time
// rather than in the network's small pieces. Taken as the upload starts, before its file is opened, and freed as it
// ends. With nothrow new: the std::vector::resize it was before aborted the device on a heap without 4 KB in one
// block (-fno-exceptions), where the upload is now refused and leaves nothing on the card.
class UploadBuffer {
 public:
  static constexpr size_t kBytes = 4096;

  // The block, kept from the upload before or taken now, and empty. False when the heap has no block for it.
  bool take() {
    used_ = 0;
    if (!data_) data_ = makeUniqueNoThrow<uint8_t[]>(kBytes);
    return data_ != nullptr;
  }
  void release() {
    data_.reset();
    used_ = 0;
  }

  // Copies what fits of `len` bytes at `src` and says how many that was. Only after take(). Once full(), the caller
  // writes data() out and clear()s it.
  size_t fill(const uint8_t* src, const size_t len) {
    const size_t n = std::min(len, kBytes - used_);
    memcpy(data_.get() + used_, src, n);
    used_ += n;
    return n;
  }
  bool full() const { return used_ == kBytes; }
  const uint8_t* data() const { return data_.get(); }
  size_t size() const { return used_; }
  void clear() { used_ = 0; }

 private:
  std::unique_ptr<uint8_t[]> data_;
  size_t used_ = 0;
};
