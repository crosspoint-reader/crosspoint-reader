// Symbols the firmware declares but expects another translation unit to define.
// In the on-device build these come from ESP-IDF / Arduino-ESP32 / a SIMULATOR-
// gated block in lib/Logging/Logging.cpp. The native simulator build needs its
// own definitions to satisfy the linker.

#include <cstdint>

#include <Logging.h>

// ---------------------------------------------------------------------------
// MySerialImpl
//
// Logging.h declares `static MySerialImpl instance;` plus several virtual
// overrides, and `#define Serial MySerialImpl::instance` redirects every
// `Serial.*` call site through it. The matching definitions used to live in
// lib/Logging/Logging.cpp behind `#ifdef SIMULATOR`, but were dropped during
// an upstream merge. Restore them here so the simulator links.
MySerialImpl MySerialImpl::instance;

size_t MySerialImpl::write(uint8_t b) { return logSerial.write(b); }
size_t MySerialImpl::write(const uint8_t *buffer, size_t size) {
  return logSerial.write(buffer, size);
}
void MySerialImpl::flush() { logSerial.flush(); }

size_t MySerialImpl::printf(const char *format, ...) {
  va_list args;
  va_start(args, format);
  char buf[256];
  const int len = vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  if (len <= 0)
    return 0;
  const size_t toWrite = (static_cast<size_t>(len) < sizeof(buf))
                             ? static_cast<size_t>(len)
                             : sizeof(buf) - 1;
  return logSerial.write(reinterpret_cast<const uint8_t *>(buf), toWrite);
}

// ---------------------------------------------------------------------------
// uzlib checksum stubs
//
// The firmware bundles only `tinflate.c` from uzlib. tinflate's
// `uzlib_uncompress_chksum` references `uzlib_adler32` / `uzlib_crc32`, whose
// implementations live in `adler32.c` / `crc32.c` upstream and were not
// vendored in. Nothing in the firmware actually calls
// `uzlib_uncompress_chksum`, but the symbols still need to resolve. Provide the
// canonical reference implementations.
extern "C" uint32_t uzlib_adler32(const void *data, unsigned int length,
                                  uint32_t prev_sum) {
  const unsigned char *buf = static_cast<const unsigned char *>(data);
  unsigned int s1 = prev_sum & 0xffff;
  unsigned int s2 = prev_sum >> 16;
  while (length--) {
    s1 = (s1 + *buf++) % 65521;
    s2 = (s2 + s1) % 65521;
  }
  return (s2 << 16) | s1;
}

extern "C" uint32_t uzlib_crc32(const void *data, unsigned int length,
                                uint32_t prev_crc) {
  static const uint32_t crc32_tab[16] = {
      0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4,
      0x4db26158, 0x5005713c, 0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
      0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
  };
  const unsigned char *buf = static_cast<const unsigned char *>(data);
  uint32_t crc = prev_crc ^ 0xffffffff;
  while (length--) {
    crc = crc32_tab[(crc ^ *buf) & 0x0f] ^ (crc >> 4);
    crc = crc32_tab[(crc ^ (*buf >> 4)) & 0x0f] ^ (crc >> 4);
    buf++;
  }
  return crc ^ 0xffffffff;
}
