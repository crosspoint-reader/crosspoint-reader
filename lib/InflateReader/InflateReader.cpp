#include "InflateReader.h"

#include <cstring>
#include <type_traits>

namespace {
// Keep the deflate history window intentionally small to avoid large transient RAM use
// in the PDF/image paths while still supporting full DEFLATE distance range.
// ZIP/deflate streams can require up to 32KiB back-references.
constexpr size_t INFLATE_DICT_SIZE = 32768;
}  // namespace

// Guarantee the cast pattern in the header comment is valid.
static_assert(std::is_standard_layout<InflateReader>::value,
              "InflateReader must be standard-layout for the uzlib callback cast to work");

InflateReader::~InflateReader() { deinit(); }

bool InflateReader::init(const bool streaming) {
  deinit();  // free any previously allocated ring buffer and reset state

  if (streaming) {
    ringBuffer = static_cast<uint8_t*>(malloc(INFLATE_DICT_SIZE));
    if (!ringBuffer) return false;
    memset(ringBuffer, 0, INFLATE_DICT_SIZE);
  }

  uzlib_uncompress_init(&decomp, ringBuffer, ringBuffer ? INFLATE_DICT_SIZE : 0);
  return true;
}

void InflateReader::deinit() {
  if (ringBuffer) {
    free(ringBuffer);
    ringBuffer = nullptr;
  }
  memset(&decomp, 0, sizeof(decomp));
}

void InflateReader::setSource(const uint8_t* src, size_t len) {
  decomp.source = src;
  decomp.source_limit = src + len;
}

void InflateReader::setReadCallback(int (*cb)(struct uzlib_uncomp*)) { decomp.source_read_cb = cb; }

void InflateReader::skipZlibHeader() {
  uzlib_get_byte(&decomp);
  uzlib_get_byte(&decomp);
}

bool InflateReader::read(uint8_t* dest, size_t len) {
  // Keep dest_start aligned with the current output chunk. This keeps the
  // streaming path valid even when callers reuse a small scratch buffer.
  decomp.dest_start = dest;
  decomp.dest = dest;
  decomp.dest_limit = dest + len;

  const int res = uzlib_uncompress(&decomp);
  if (res < 0) return false;
  return decomp.dest == decomp.dest_limit;
}

InflateStatus InflateReader::readAtMost(uint8_t* dest, size_t maxLen, size_t* produced) {
  // Keep dest_start aligned with the current output chunk. This keeps the
  // streaming path valid even when callers reuse a small scratch buffer.
  decomp.dest_start = dest;
  decomp.dest = dest;
  decomp.dest_limit = dest + maxLen;

  const int res = uzlib_uncompress(&decomp);
  *produced = static_cast<size_t>(decomp.dest - dest);

  if (res == TINF_DONE) return InflateStatus::Done;
  if (res < 0) return InflateStatus::Error;
  return InflateStatus::Ok;
}
