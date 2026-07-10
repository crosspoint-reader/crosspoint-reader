#include "InflateStream.h"

#include <BuildScratch.h>

#include <cstdlib>
#include <cstring>

#include "MinizConfig.h"

namespace {
// tinfl's window must be a power of two; TINFL_LZ_DICT_SIZE is 32768.
constexpr size_t WINDOW_SIZE = TINFL_LZ_DICT_SIZE;
// tinfl_decompressor holds mz_uint32 arrays; 8 keeps the window aligned too.
constexpr size_t STATE_ALIGNED = (sizeof(tinfl_decompressor) + 7) & ~size_t{7};
}  // namespace

InflateStream::~InflateStream() { deinit(); }

bool InflateStream::init(const bool streaming) {
  // Every consumer constructs a fresh stream per operation, so acquire storage
  // from scratch each init (releasing any prior backing first).
  deinit();

  // During a framebuffer loan the lent 48KB is up for grabs: state (~11KB) +
  // window (32KB) fit inside it, so a chapter-build inflate costs the heap
  // nothing. Absent (or already claimed): plain heap, freed in deinit().
  const size_t needed = STATE_ALIGNED + (streaming ? WINDOW_SIZE : 0);
  arenaBase = buildscratch::claim(needed);
  if (arenaBase) {
    state = reinterpret_cast<tinfl_decompressor*>(arenaBase);
    window = streaming ? arenaBase + STATE_ALIGNED : nullptr;
  } else {
    // Raw malloc (not makeUniqueNoThrow): the header keeps tinfl_decompressor
    // an incomplete type so consumers never include miniz; both blocks are
    // freed in deinit()/the destructor.
    state = static_cast<tinfl_decompressor*>(malloc(sizeof(tinfl_decompressor)));
    if (!state) return false;
    if (streaming) {
      window = static_cast<uint8_t*>(malloc(WINDOW_SIZE));
      if (!window) return false;  // state kept; deinit()/next init reclaims it
    }
  }

  tinfl_init(state);
  windowPos = 0;
  pendingStart = 0;
  pendingLen = 0;
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
  if (arenaBase) {
    buildscratch::release(arenaBase);
    arenaBase = nullptr;
  } else {
    free(state);
    free(window);
  }
  state = nullptr;
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

  const bool streaming = window != nullptr;
  if (!streaming && !oneShotStart) oneShotStart = dest;

  for (;;) {
    // Drain window bytes left over from a previous tinfl call. In ring mode
    // tinfl may produce more than the caller asked for in one shot -- the
    // overshoot stays pending in the window until a later readAtMost.
    if (pendingLen > 0) {
      size_t n = maxLen - *produced;
      if (n > pendingLen) n = pendingLen;
      memcpy(dest + *produced, window + pendingStart, n);
      pendingStart += n;
      pendingLen -= n;
      *produced += n;
    }
    if (*produced == maxLen) {
      return (finished && pendingLen == 0) ? Status::Done : Status::Ok;
    }
    if (finished) return Status::Done;

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
      // Ring mode invariant: tinfl derives its wrap mask from
      // (cursor offset + avail_out), so avail_out MUST always reach the end of
      // the 32KB window -- never cap it to the caller's remaining space.
      outBytes = WINDOW_SIZE - windowPos;
      status = tinfl_decompress(state, inPtr, &inBytes, window, window + windowPos, &outBytes, flags);
      pendingStart = windowPos;
      pendingLen = outBytes;
      windowPos += outBytes;
      if (windowPos == WINDOW_SIZE) windowPos = 0;
    } else {
      // One-shot: back-references resolve directly inside the destination buffer.
      outBytes = maxLen - *produced;
      status = tinfl_decompress(state, inPtr, &inBytes, oneShotStart, dest + *produced, &outBytes, flags);
      *produced += outBytes;
    }
    inPtr += inBytes;
    inAvail -= inBytes;

    if (status == TINFL_STATUS_DONE) {
      finished = true;  // drain any pending window bytes on the next pass
      continue;
    }
    if (status < TINFL_STATUS_DONE) return Status::Error;  // corrupt stream / adler mismatch
    // TINFL_STATUS_NEEDS_MORE_INPUT loops back to the fill above; once the fill
    // runs dry the HAS_MORE_INPUT flag drops and tinfl either finishes or fails
    // (truncated stream) instead of spinning.
    if (status == TINFL_STATUS_NEEDS_MORE_INPUT && inputExhausted && inAvail == 0) {
      return Status::Error;
    }
    if (*produced == maxLen) {
      return (finished && pendingLen == 0) ? Status::Done : Status::Ok;
    }
  }
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
