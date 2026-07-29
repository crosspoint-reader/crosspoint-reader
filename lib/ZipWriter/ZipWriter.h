#pragma once

// Minimal ZIP writer: stored (uncompressed) entries only. Purpose-built for
// on-device EPUB generation (MOBI/PDF converters): EPUB requires a ZIP
// container but gains nothing from deflate on an SD card, and stored entries
// keep the writer tiny and the reader's inflate path untouched.
//
// Usage: begin() -> addFile() per entry (EPUB: "mimetype" first) -> finish().
// Entries are buffered by the caller; addFile writes straight through, so
// peak RAM is one entry, not the archive.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class HalFile;

class ZipWriter {
 public:
  ZipWriter();
  ~ZipWriter();

  bool begin(const char* path);
  bool addFile(const char* name, const uint8_t* data, size_t len);
  // Streaming variant: fixed-size entry fed in chunks. Call beginStreamedFile,
  // then writeChunk repeatedly, then endStreamedFile. Total written must equal
  // the declared size (the local header is written up front — stored entries
  // need the size before the data).
  bool beginStreamedFile(const char* name, uint32_t totalLen);
  bool writeChunk(const uint8_t* data, size_t len);
  bool endStreamedFile();
  bool finish();
  // Close the output without writing a central directory (failed conversion).
  // Must be called before removing the file — SdFat must not have it open.
  void abort();

 private:
  struct Entry {
    std::string name;
    uint32_t crc;
    uint32_t size;
    uint32_t offset;
  };

  bool writeLocalHeader(const char* name, uint32_t crc, uint32_t size);

  std::unique_ptr<HalFile> out;
  std::vector<Entry> entries;
  uint32_t offset = 0;

  // streaming state
  bool streaming = false;
  uint32_t streamDeclared = 0;
  uint32_t streamWritten = 0;
  uint32_t streamCrc = 0;
  uint32_t streamHeaderOffset = 0;
};
