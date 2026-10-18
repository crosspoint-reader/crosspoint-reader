#pragma once
#include <stddef.h>
#include <stdint.h>
namespace usb_update {
enum class Error { None, File, Address, Write, Verify, Unavailable };
struct Sink {
  virtual ~Sink() = default;
  virtual bool begin(uint32_t size, const uint8_t sha256[32]) = 0;
  virtual bool beginCompressed(uint32_t, const uint8_t[32]) { return false; }
  virtual bool append(const uint8_t* data, size_t size) = 0;
  virtual Error finish() = 0;
  virtual void abort() = 0;
  // Storage/display work runs after the busy status handshake. Let the sink
  // advertise the budget for that particular operation before it begins.
  virtual uint32_t pollTimeout(bool, uint32_t, uint32_t, uint16_t) const { return UINT32_MAX; }
};
class Session {
  Sink& sink;
  uint32_t limit, minimum, manifestWait, flushWait, total = 0, received = 0;
  uint16_t blockSize, maximumBlock;
  uint16_t next = 0;
  bool header = false, done = false, opened = false;
  Error failure = Error::None;
  Error fail(Error error);

 public:
  Session(Sink& target, uint32_t maxSize, uint32_t minSize = 65536, uint32_t finalWaitMs = 120000,
          uint32_t flushWaitMs = 1000, uint16_t transferSize = 64)
      : sink(target),
        limit(maxSize),
        minimum(minSize),
        manifestWait(finalWaitMs),
        flushWait(flushWaitMs),
        blockSize(transferSize),
        maximumBlock(transferSize) {}
  Error download(uint16_t block, const uint8_t* data, size_t size);
  Error manifest();
  void abort();
  uint32_t pollTimeout(bool manifestation) const;
  uint32_t size() const { return total; }
  uint32_t bytes() const { return received; }
  uint16_t expectedBlock() const { return next; }
  bool complete() const { return done; }
  bool started() const { return header; }
  Error error() const { return failure; }
};
}  // namespace usb_update
