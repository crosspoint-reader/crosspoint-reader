#include "InflateReader.h"

#include <cstring>
#include <type_traits>

namespace {
constexpr size_t INFLATE_DICT_SIZE = InflateReader::RING_BYTES;
}

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
    streamingMode = true;
  }

  uzlib_uncompress_init(&decomp, ringBuffer, ringBuffer ? INFLATE_DICT_SIZE : 0);
  return true;
}

bool InflateReader::initWithSegments(uint8_t* const (&segments)[RING_SEGMENTS]) {
  static_assert((RING_SEGMENT_BYTES & (RING_SEGMENT_BYTES - 1)) == 0, "uzlib indexes segments by shift/mask");
  static_assert(RING_SEGMENTS * RING_SEGMENT_BYTES == RING_BYTES, "segments must cover the full deflate window");
  static_assert(RING_SEGMENTS <= UZLIB_DICT_MAX_SEGS, "uzlib segment table too small");

  deinit();  // free any owned ring buffer and reset state
  for (uint8_t* segment : segments) {
    if (!segment) return false;
  }
  for (uint8_t* segment : segments) memset(segment, 0, RING_SEGMENT_BYTES);
  uzlib_uncompress_init_segmented(&decomp, segments, RING_SEGMENTS, __builtin_ctz(RING_SEGMENT_BYTES));
  streamingMode = true;
  return true;
}

void InflateReader::deinit() {
  free(ringBuffer);
  ringBuffer = nullptr;
  streamingMode = false;
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
  if (!streamingMode) {
    // One-shot mode: back-references use absolute offset from dest_start.
    // Valid only when read() is called once with the full output buffer.
    decomp.dest_start = dest;
  }
  decomp.dest = dest;
  decomp.dest_limit = dest + len;

  const int res = uzlib_uncompress(&decomp);
  if (res < 0) return false;
  return decomp.dest == decomp.dest_limit;
}

InflateStatus InflateReader::readAtMost(uint8_t* dest, size_t maxLen, size_t* produced) {
  if (!streamingMode) {
    // One-shot mode: back-references use absolute offset from dest_start.
    // Valid only when readAtMost() is called once with the full output buffer.
    decomp.dest_start = dest;
  }
  decomp.dest = dest;
  decomp.dest_limit = dest + maxLen;

  const int res = uzlib_uncompress(&decomp);
  *produced = static_cast<size_t>(decomp.dest - dest);

  if (res == TINF_DONE) return InflateStatus::Done;
  if (res < 0) return InflateStatus::Error;
  return InflateStatus::Ok;
}
