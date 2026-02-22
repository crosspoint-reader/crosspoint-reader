#pragma once

#include <uzlib.h>

#include <cstddef>

// Return value for readAtMost().
enum class InflateStatus {
  Ok,     // Output buffer full; more compressed data remains.
  Done,   // Stream ended cleanly (TINF_DONE). produced may be < maxLen.
  Error,  // Decompression failed.
};

// Streaming deflate decompressor wrapping uzlib.
//
// Two modes:
//   init(false)  — one-shot: input is a contiguous buffer, call read() once.
//   init(true)   — streaming: allocates a 32KB ring buffer for back-references
//                  across multiple read() / readAtMost() calls.
//
// Streaming callback pattern:
//   The uzlib read callback receives a `struct uzlib_uncomp*` with no separate
//   context pointer. To attach context, make InflateReader the *first member* of
//   your context struct, then cast inside the callback:
//
//     struct MyCtx {
//       InflateReader reader;   // must be first
//       FsFile* file;
//       // ...
//     };
//     static int myCb(struct uzlib_uncomp* u) {
//       MyCtx* ctx = reinterpret_cast<MyCtx*>(u);   // valid: reader.decomp is at offset 0
//       // ... fill u->source / u->source_limit, return first byte
//     }
//     MyCtx ctx;
//     ctx.reader.init(true);
//     ctx.reader.setReadCallback(myCb);
//
class InflateReader {
 public:
  InflateReader() = default;
  ~InflateReader();

  InflateReader(const InflateReader&) = delete;
  InflateReader& operator=(const InflateReader&) = delete;

  // Initialise decompressor. streaming=true allocates a 32KB ring buffer needed
  // when read() or readAtMost() will be called multiple times.
  // Returns false only in streaming mode if the ring buffer allocation fails.
  bool init(bool streaming = false);

  // Release the ring buffer and reset internal state.
  void deinit();

  // Set the entire compressed input as a contiguous memory buffer.
  // Used in one-shot mode; not needed when a read callback is set.
  void setSource(const uint8_t* src, size_t len);

  // Set a uzlib-compatible read callback for streaming input.
  // See class-level comment for the expected callback/context struct pattern.
  void setReadCallback(int (*cb)(uzlib_uncomp*));

  // Consume the 2-byte zlib header (CMF + FLG) from the input stream.
  // Call this once before the first read() when input is zlib-wrapped (e.g. PNG IDAT).
  void skipZlibHeader();

  // Decompress exactly len bytes into dest.
  // Returns false if the stream ends before producing len bytes, or on error.
  bool read(uint8_t* dest, size_t len);

  // Decompress up to maxLen bytes into dest.
  // Sets *produced to the number of bytes written.
  // Returns Done when the stream ends cleanly, Ok when there is more to read,
  // and Error on failure.
  InflateStatus readAtMost(uint8_t* dest, size_t maxLen, size_t* produced);

  // Returns a pointer to the underlying TINF_DATA.
  // Useful for advanced streaming setups where the callback needs access to the
  // uzlib struct directly (e.g. updating source/source_limit).
  uzlib_uncomp* raw() { return &decomp; }

 private:
  uzlib_uncomp decomp = {};
  uint8_t* ringBuffer = nullptr;
};
