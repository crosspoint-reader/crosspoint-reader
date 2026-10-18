#include "DfuSession.h"

#include <string.h>
namespace usb_update {
Error Session::fail(Error e) {
  failure = e;
  if (opened) {
    sink.abort();
    opened = false;
  }
  return e;
}
Error Session::download(uint16_t block, const uint8_t* data, size_t size) {
  if (done || failure != Error::None) return Error::Unavailable;
  if (block != next || !data || !size || size > blockSize) return fail(Error::Address);
  if (!header) {
    if (size != 64) return fail(Error::File);
    const bool compressed = !memcmp(data, "X3DFU002", 8);
    if (!compressed && memcmp(data, "X3DFU001", 8)) return fail(Error::File);
    for (unsigned i = compressed ? 48 : 44; i < 64; i++)
      if (data[i]) return fail(Error::File);
    total = uint32_t(data[8]) | (uint32_t(data[9]) << 8) | (uint32_t(data[10]) << 16) | (uint32_t(data[11]) << 24);
    if (compressed) {
      const uint32_t raw =
          uint32_t(data[44]) | (uint32_t(data[45]) << 8) | (uint32_t(data[46]) << 16) | (uint32_t(data[47]) << 24);
      if (raw < minimum || raw > limit || !total || total % 4096 || total > ((uint64_t(raw) + 32759) / 32760) * 32768 ||
          maximumBlock != 4096)
        return fail(Error::Address);
      if (!sink.beginCompressed(raw, data + 12)) {
        sink.abort();
        return fail(Error::Write);
      }
    } else {
      if (total < minimum || total > limit) return fail(Error::Address);
      if (!sink.begin(total, data + 12)) {
        sink.abort();
        return fail(Error::Write);
      }
    }
    opened = header = true;
    next = 1;
    return Error::None;
  }
  // The first payload may select a smaller whole-USB-packet block size for
  // host controllers that need it. Keep that size fixed for the session.
  if (!received && size >= 64 && size % 64 == 0) blockSize = uint16_t(size);
  if (size > total - received || (size != blockSize && size != total - received)) return fail(Error::Address);
  if (!sink.append(data, size)) return fail(Error::Write);
  received += size;
  next = uint16_t(next + 1);
  return Error::None;
}
Error Session::manifest() {
  if (done) return Error::None;
  if (failure != Error::None) return failure;
  if (!header || received != total) return fail(Error::File);
  Error result = sink.finish();
  if (result != Error::None) return fail(result);
  opened = false;
  done = true;
  return Error::None;
}
void Session::abort() {
  // A successful manifest may already have selected the new boot partition.
  // A later host cleanup request must not erase that fact or reopen the sink.
  if (done) return;
  if (opened) sink.abort();
  opened = header = false;
  total = received = 0;
  next = 0;
  blockSize = maximumBlock;
  failure = Error::None;
}
uint32_t Session::pollTimeout(bool manifestation) const {
  const uint32_t storageWait = sink.pollTimeout(manifestation, received, total, blockSize);
  if (storageWait != UINT32_MAX) return storageWait;
  if (manifestation) return manifestWait;                      // SD verification, flash write/readback, boot selection.
  if (!header) return 1000;                                    // SD file creation.
  return received % 4096 + blockSize >= 4096 ? flushWait : 2;  // The sink buffers writes in 4 KiB chunks.
}
}  // namespace usb_update
