#include "InflateStream.h"

#include <cstdlib>
#include <cstring>

#include "MinizConfig.h"

namespace {
// tinfl's window must be a power of two; TINFL_LZ_DICT_SIZE is 32768.
constexpr size_t WINDOW_SIZE = TINFL_LZ_DICT_SIZE;
}  // namespace

InflateStream::~InflateStream() { deinit(); }

bool InflateStream::init(const bool streaming) {
  // Raw malloc (not makeUniqueNoThrow): the header keeps tinfl_decompressor an
  // incomplete type so consumers never include miniz; both blocks are freed in
  // deinit()/the destructor, and there is no early-return between here and use.
  if (!state) {
    state = static_cast<tinfl_decompressor*>(malloc(sizeof(tinfl_decompressor)));
    if (!state) return false;
  }
  if (streaming && !window) {
    window = static_cast<uint8_t*>(malloc(WINDOW_SIZE));
    if (!window) return false;  // state kept; a later init() reuses it
  }
  if (!streaming && window) {
    free(window);
    window = nullptr;
  }

  tinfl_init(state);
  windowPos = 0;
  inPtr = nullptr;
  inAvail = 0;
  fill = nullptr;
  fillCtx = nullptr;
  inputExhausted = false;
  zlibWrapped = false;
  finished = false;
  oneShotStart = nullptr;
  return true;
}

void InflateStream::deinit() {
  free(state);
  state = nullptr;
  free(window);
  window = nullptr;
}

void InflateStream::setSource(const uint8_t* src, const size_t len) {
  inPtr = src;
  inAvail = len;
  inputExhausted = true;  // the whole input is present; nothing more will come
}

void InflateStream::setFill(const FillFn fn, void* ctx) {
  fill = fn;
  fillCtx = ctx;
}

InflateStream::Status InflateStream::readAtMost(uint8_t* dest, const size_t maxLen, size_t* produced) {
  *produced = 0;
  if (!state) return Status::Error;
  if (finished) return Status::Done;

  const bool streaming = window != nullptr;
  if (!streaming && !oneShotStart) oneShotStart = dest;

  while (*produced < maxLen) {
    if (inAvail == 0 && !inputExhausted && fill) {
      inAvail = fill(fillCtx, &inPtr);
      if (inAvail == 0) inputExhausted = true;
    }

    const mz_uint32 flags = (zlibWrapped ? TINFL_FLAG_PARSE_ZLIB_HEADER : 0) |
                            (inputExhausted ? 0 : TINFL_FLAG_HAS_MORE_INPUT) |
                            (streaming ? 0 : TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);

    size_t inBytes = inAvail;
    tinfl_status status;
    size_t outBytes;
    if (streaming) {
      // Decompress into the ring window, then copy the fresh bytes out to dest.
      outBytes = WINDOW_SIZE - windowPos;
      if (outBytes > maxLen - *produced) outBytes = maxLen - *produced;
      status = tinfl_decompress(state, inPtr, &inBytes, window, window + windowPos, &outBytes, flags);
      memcpy(dest + *produced, window + windowPos, outBytes);
      windowPos = (windowPos + outBytes) & (WINDOW_SIZE - 1);
    } else {
      // One-shot: back-references resolve directly inside the destination buffer.
      outBytes = maxLen - *produced;
      status = tinfl_decompress(state, inPtr, &inBytes, oneShotStart, dest + *produced, &outBytes, flags);
    }
    inPtr += inBytes;
    inAvail -= inBytes;
    *produced += outBytes;

    if (status == TINFL_STATUS_DONE) {
      finished = true;
      return Status::Done;
    }
    if (status < TINFL_STATUS_DONE) return Status::Error;  // corrupt stream / adler mismatch
    // TINFL_STATUS_NEEDS_MORE_INPUT loops back to the fill above; once the fill
    // runs dry the HAS_MORE_INPUT flag drops and tinfl either finishes or fails
    // (truncated stream) instead of spinning.
    if (status == TINFL_STATUS_NEEDS_MORE_INPUT && inputExhausted && inAvail == 0) {
      return Status::Error;
    }
  }
  return Status::Ok;
}

bool InflateStream::read(uint8_t* dest, const size_t len) {
  size_t total = 0;
  while (total < len) {
    size_t produced = 0;
    const Status status = readAtMost(dest + total, len - total, &produced);
    total += produced;
    if (status == Status::Error) return false;
    if (status == Status::Done) return total == len;
    if (produced == 0) return false;  // no progress safeguard
  }
  return true;
}
