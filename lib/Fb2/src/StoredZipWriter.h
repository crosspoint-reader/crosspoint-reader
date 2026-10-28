#pragma once

// Minimal ZIP writer producing entries with no compression (method 0 /
// "stored"). Used to build the synthetic EPUB container for FB2 books:
// the firmware's ZipFile reader supports stored entries, so the converted
// book needs no compressor on-device.
//
// Local file headers are written with placeholder sizes/CRC and rewritten
// in place once the entry data is complete, so the output is a plain,
// descriptor-free ZIP that any reader can open.

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class StoredZipWriter {
 public:
  bool open(const std::string& path);
  // Begin an entry. No nested entries: finishEntry() must be called first.
  bool startEntry(const char* name);
  // Append entry data.
  bool write(const void* data, size_t len);
  bool writeByte(char c) { return write(&c, 1); }
  // Convenience for C-strings (no terminator).
  bool writeStr(const char* s);
  // printf-style chunked write into a fixed buffer (max 512 bytes per call).
  bool writeFmt(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
  bool finishEntry();
  // Write the central directory and the end-of-central-directory record.
  bool finish();
  void abort();

  bool ok() const { return ok_; }
  uint32_t entryCount() const { return static_cast<uint32_t>(entries_.size()); }

  static constexpr size_t kMaxEntries = 4096;
  static constexpr size_t kMaxNameLength = 96;

 private:
  struct CentralEntry {
    char name[kMaxNameLength + 1] = {};
    uint32_t crc = 0;
    uint32_t size = 0;
    uint32_t offset = 0;  // local header offset
  };

  bool writeAll(const void* data, size_t len);
  bool writeLocalHeader(uint32_t crc, uint32_t size);
  void flushIo();

  HalFile file_;
  std::vector<CentralEntry> entries_;
  // Buffered output: entry data arrives in tiny (often single-byte) pieces, and
  // per-byte SD writes make a book-sized conversion take minutes. All writes go
  // through this buffer; flushes happen on demand, before seeks, and at close.
  uint8_t ioBuf_[4096] = {};
  size_t ioLen_ = 0;
  uint64_t filePos_ = 0;  // bytes actually written to the file (buffer flushed)
  // Current entry state
  uint32_t headerOffset_ = 0;
  uint32_t entrySize_ = 0;
  uint32_t entryCrc_ = 0;
  bool entryOpen_ = false;
  uint64_t bytesWritten_ = 0;  // logical position (includes buffered bytes)
  bool ok_ = false;
  bool started_ = false;

  static uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t len);
};
