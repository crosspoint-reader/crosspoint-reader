#pragma once

#include <cstddef>
#include <cstdint>

// Board-identity tag embedded in every CrossPoint image, plus a streaming
// scanner the firmware update paths use to reject an image built for a
// different board before it can boot and drive another device's pins. All the
// S3 boards (sticky, x4pro, papermono, ...) share a chip_id, so the existing
// esp_image_header chip check cannot tell them apart.
//
// The tag is "CROSSPOINT-BOARD-V1:<board>;" stored once in .rodata — the
// scanner's needle references the same array, so a CrossPoint image contains
// exactly one occurrence. Images without a tag (other projects, forks, older
// releases) are allowed: the guard only rejects a tag naming a DIFFERENT
// board.

namespace board_tag {

// Full tag: magic prefix + board name + ';'.
extern const char TAG[];

// Board name of the running firmware (pointer into TAG; not null-terminated at
// the name boundary — always pair with boardNameLen()).
const char* boardName();
size_t boardNameLen();

// Incremental scanner: feed every byte of a candidate image in stream order,
// then check mismatch(). State persists across feed() calls, so chunk
// boundaries splitting the tag are handled.
class Scanner {
 public:
  void feed(const uint8_t* data, size_t len);
  // True once a tag naming a different board has been seen. Valid mid-stream:
  // callers may abort a download as soon as this turns true.
  bool mismatch() const { return mismatchFound; }
  // Board name from the offending tag, for logging (empty until mismatch()).
  const char* foundName() const { return mismatchFound ? captured : ""; }

 private:
  static constexpr size_t MAX_NAME = 23;
  char captured[MAX_NAME + 1] = {0};
  size_t nameLen = 0;
  size_t magicMatched = 0;
  bool capturing = false;
  bool mismatchFound = false;
};

}  // namespace board_tag
