#pragma once
#include <HalStorage.h>
#include <Serialization.h>

#include <cstring>
#include <memory>
#include <string>

// Buffered wrappers over FsFile for the record-stream files the book indexer uses
// (.items.bin, spine/toc temp files, book.bin). Their access pattern is thousands of
// tiny field reads/writes (a u32 length here, a 40-byte string there); issued directly
// each one is a full FsFile call at ~1.5 ms on SD, which made a 1732-spine first-open
// index spend ~28 s in I/O overhead alone. These classes batch the fields into
// buffer-sized transfers (default 4 KB) — the wire format is exactly Serialization.h's,
// so buffered writers/readers interoperate freely with the unbuffered functions.
//
// Allocation: one heap buffer per instance (default 4 KB; heap because the instances
// live across parse callbacks and 4 KB exceeds the stack budget). On allocation failure
// the instance degrades to unbuffered pass-through — never an error path for callers.
//
// Not general-purpose file abstractions: no interleaved read/write on one instance, and
// the underlying FsFile must not be repositioned behind the wrapper's back while it is
// in use (position()/seek() go through the wrapper).

namespace serialization {

class BufferedFileReader {
 public:
  static constexpr size_t DEFAULT_BUFFER_BYTES = 4096;

  explicit BufferedFileReader(FsFile& file, const size_t bufferBytes = DEFAULT_BUFFER_BYTES) : file_(file) {
    // nothrow new, owned by the unique_ptr: null means degrade to pass-through, never abort.
    buf_.reset(new (std::nothrow) uint8_t[bufferBytes]);
    cap_ = buf_ ? bufferBytes : 0;
    windowStart_ = static_cast<uint32_t>(file_.position());
  }

  // Caller-supplied buffer, NOT owned, as BufferedFileWriter takes one: for a reader whose buffer
  // comes out of a lent region, or is reused across several readers. A null buffer degrades to
  // pass-through exactly as a failed allocation does. The buffer must outlive this reader.
  BufferedFileReader(FsFile& file, uint8_t* const buffer, const size_t bufferBytes)
      : file_(file), external_(buffer), cap_(buffer ? bufferBytes : 0) {
    windowStart_ = static_cast<uint32_t>(file_.position());
  }

  // Logical read position (the underlying file may be ahead by the buffered remainder).
  uint32_t position() const { return windowStart_ + static_cast<uint32_t>(pos_); }

  // Repositions the logical cursor. A target inside the current window is free (no I/O);
  // anything else drops the window and seeks the file.
  void seek(const uint32_t target) {
    if (target >= windowStart_ && target <= windowStart_ + fill_) {
      pos_ = target - windowStart_;
      return;
    }
    file_.seek(target);
    windowStart_ = target;
    pos_ = 0;
    fill_ = 0;
  }

  // Reads exactly n bytes; false on a short read (logical position still advances by the
  // bytes actually consumed).
  bool read(void* dst, size_t n) {
    auto* out = static_cast<uint8_t*>(dst);
    while (n > 0) {
      const size_t avail = fill_ - pos_;
      if (avail > 0) {
        const size_t take = n < avail ? n : avail;
        memcpy(out, bufferBase() + pos_, take);
        pos_ += take;
        out += take;
        n -= take;
        continue;
      }
      // Window exhausted. Serve large remainders directly (no point copying through the
      // buffer); refill for small ones. cap_ == 0 (degraded) always takes the direct path.
      windowStart_ += static_cast<uint32_t>(fill_);
      pos_ = 0;
      fill_ = 0;
      if (n >= cap_) {
        const int got = file_.read(out, n);
        if (got > 0) windowStart_ += static_cast<uint32_t>(got);
        return got == static_cast<int>(n);
      }
      const int got = file_.read(bufferBase(), cap_);
      if (got <= 0) return false;
      fill_ = static_cast<size_t>(got);
    }
    return true;
  }

  template <typename T>
  bool readPod(T& value) {
    return read(&value, sizeof(T));
  }

  // Wire-format and failure parity with serialization::readString(FsFile&, ...): an
  // oversized length skips the payload (stream stays aligned) and returns false.
  bool readString(std::string& s) {
    uint32_t len = 0;
    if (!readPod(len)) return false;
    if (len > MAX_STRING_LENGTH) {
      seek(position() + len);
      return false;
    }
    s.resize(len);
    return len == 0 || read(&s[0], len);
  }

  // readString() of the record at `offset`, for random access between sequential stretches: from
  // the window when the record lies inside it, else straight from the file (a seek and two reads,
  // not a window's worth). The window and the logical position are left as they were.
  bool readStringAt(const uint32_t offset, std::string& s) {
    uint32_t len = 0;
    if (offset >= windowStart_ && offset + sizeof(len) <= windowStart_ + fill_) {
      memcpy(&len, bufferBase() + (offset - windowStart_), sizeof(len));
      if (len <= MAX_STRING_LENGTH && offset + sizeof(len) + len <= windowStart_ + fill_) {
        s.assign(reinterpret_cast<const char*>(bufferBase() + (offset - windowStart_) + sizeof(len)), len);
        return true;
      }
    }
    file_.seek(offset);
    const bool ok = serialization::readString(file_, s);
    // Where the window's next refill reads from.
    file_.seek(windowStart_ + fill_);
    return ok;
  }

 private:
  // Owned or borrowed, never both: the owning constructor leaves external_ null and vice versa.
  uint8_t* bufferBase() const { return external_ ? external_ : buf_.get(); }

  FsFile& file_;
  std::unique_ptr<uint8_t[]> buf_;
  uint8_t* external_ = nullptr;
  size_t cap_ = 0;
  size_t fill_ = 0;           // valid bytes in the buffer
  size_t pos_ = 0;            // consumed bytes in the buffer
  uint32_t windowStart_ = 0;  // file offset of the buffer's first byte
};

class BufferedFileWriter {
 public:
  static constexpr size_t DEFAULT_BUFFER_BYTES = 4096;

  explicit BufferedFileWriter(FsFile& file, const size_t bufferBytes = DEFAULT_BUFFER_BYTES) : file_(file) {
    // nothrow new, owned by the unique_ptr: null means degrade to pass-through, never abort.
    buf_.reset(new (std::nothrow) uint8_t[bufferBytes]);
    cap_ = buf_ ? bufferBytes : 0;
    base_ = static_cast<uint32_t>(file_.position());
  }

  // Caller-supplied buffer, NOT owned — for paths that already hold scratch worth reusing (the
  // image extract carves it out of the borrowed framebuffer arena rather than asking a heap that
  // is, at that exact moment, about to be handed to a decoder). A null buffer degrades to
  // pass-through exactly as a failed allocation does. The buffer must outlive this writer.
  BufferedFileWriter(FsFile& file, uint8_t* const buffer, const size_t bufferBytes)
      : file_(file), external_(buffer), cap_(buffer ? bufferBytes : 0) {
    base_ = static_cast<uint32_t>(file_.position());
  }

  // Callers are expected to flush() at the end of a write phase (and check its result);
  // the destructor flush is a best-effort backstop only.
  ~BufferedFileWriter() { flush(); }

  // Logical write position (== what FsFile::position() would report after a flush).
  uint32_t position() const { return base_ + static_cast<uint32_t>(fill_); }

  bool write(const void* src, size_t n) {
    const auto* in = static_cast<const uint8_t*>(src);
    // Large payloads (or degraded mode) go straight to the file, after draining the buffer
    // so ordering is preserved.
    if (n >= cap_) {
      if (!flush()) return false;
      const size_t wrote = file_.write(in, n);
      base_ += static_cast<uint32_t>(wrote);
      return wrote == n;
    }
    if (fill_ + n > cap_ && !flush()) return false;
    memcpy(bufferBase() + fill_, in, n);
    fill_ += n;
    return true;
  }

  template <typename T>
  bool writePod(const T& value) {
    return write(&value, sizeof(T));
  }

  bool writeString(const std::string& s) {
    const uint32_t len = static_cast<uint32_t>(s.size());
    return writePod(len) && write(s.data(), len);
  }

  bool flush() {
    if (fill_ == 0) return true;
    const size_t wrote = file_.write(bufferBase(), fill_);
    base_ += static_cast<uint32_t>(wrote);
    const bool ok = wrote == fill_;
    fill_ = 0;
    return ok;
  }

 private:
  // Owned or borrowed, never both: the owning constructor leaves external_ null and vice versa.
  uint8_t* bufferBase() const { return external_ ? external_ : buf_.get(); }

  FsFile& file_;
  std::unique_ptr<uint8_t[]> buf_;
  uint8_t* external_ = nullptr;
  size_t cap_ = 0;
  size_t fill_ = 0;
  uint32_t base_ = 0;  // file offset the buffer starts at
};

}  // namespace serialization
