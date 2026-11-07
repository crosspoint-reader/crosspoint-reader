#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>

// Bounded incremental odc/newc parser. The caller owns storage and runs callbacks
// on its own task. Complete earlier files may survive a later archive failure.
class AdCpioStream {
 public:
  struct Sink {
    void* ctx;
    bool (*begin)(void*, const char*, uint32_t);
    bool (*write)(void*, const uint8_t*, uint32_t);
    bool (*end)(void*);
  };
  void reset(Sink next, uint32_t limit) {
    *this = AdCpioStream();
    sink = next;
    ceiling = limit;
  }
  bool feed(const uint8_t* data, uint32_t len) {
    if (state == State::Failed) return false;
    if (len > ceiling - total) {
      oversized = true;
      return fail();
    }
    total += len;
    while (len) {
      switch (state) {
        case State::Header: {
          const auto n = std::min(len, need - used);
          memcpy(header + used, data, n);
          used += n;
          advance(data, len, n);
          if (used != need) break;
          if (need == 6) {
            if (!memcmp(header, "070707", 6)) {
              aligned = false;
              need = 76;
            } else if (!memcmp(header, "070701", 6)) {
              aligned = true;
              need = 110;
            } else
              return fail();  // CRC-newc is not accepted without checksum validation.
          } else {
            if (!number(aligned ? 14 : 18, aligned ? 8 : 6, mode) ||
                !number(aligned ? 54 : 65, aligned ? 8 : 11, size) ||
                !number(aligned ? 94 : 59, aligned ? 8 : 6, nameSize) || nameSize == 0 || nameSize > sizeof(name))
              return fail();
            if (size > ceiling) {
              oversized = true;
              return fail();
            }
            state = State::Name;
            used = 0;
          }
          break;
        }
        case State::Name: {
          const auto n = std::min(len, nameSize - used);
          memcpy(name + used, data, n);
          used += n;
          advance(data, len, n);
          if (used != nameSize) break;
          if (name[nameSize - 1] != 0 || memchr(name, 0, nameSize - 1)) return fail();
          if (!strcmp(name, "TRAILER!!!")) {
            if (size) return fail();
            state = State::Trailer;
            break;
          }
          regular = mode == 0 || (mode & 0170000u) == 0100000u;
          if (regular) {
            const char* base = strrchr(name, '/');
            base = base ? base + 1 : name;
            if (!*base || strlen(base) > 255 || !sink.begin(sink.ctx, base, size)) return fail();
          }
          remaining = size;
          padding = aligned ? (4 - (offset & 3)) & 3 : 0;
          state = State::NamePadding;
          break;
        }
        case State::NamePadding:
          if (!skip(data, len)) break;
          state = State::Data;
          if (!remaining && !endFile()) return false;
          break;
        case State::Data: {
          const auto n = std::min(len, remaining);
          if (regular && n && !sink.write(sink.ctx, data, n)) return fail();
          advance(data, len, n);
          remaining -= n;
          if (!remaining && !endFile()) return false;
          break;
        }
        case State::DataPadding:
          if (!skip(data, len)) break;
          state = State::Header;
          used = 0;
          need = 6;
          break;
        case State::Trailer:
          // cpio pads its final block with zero bytes only.
          while (len) {
            if (*data) return fail();
            advance(data, len, 1);
          }
          break;
        case State::Failed:
          return false;
      }
    }
    // Zero-byte files and zero padding must progress even at a chunk boundary.
    if (state == State::NamePadding && !padding) {
      state = State::Data;
      if (!remaining && !endFile()) return false;
    }
    return true;
  }
  bool complete() const { return state == State::Trailer && files != 0; }
  bool tooLarge() const { return oversized; }
  uint32_t fileCount() const { return files; }

 private:
  enum class State { Header, Name, NamePadding, Data, DataPadding, Trailer, Failed };
  State state = State::Header;
  Sink sink{};
  uint8_t header[110]{};
  char name[512]{};
  uint32_t ceiling = 0, total = 0, offset = 0, used = 0, need = 6;
  uint32_t mode = 0, size = 0, nameSize = 0, remaining = 0, padding = 0, files = 0;
  bool aligned = false, regular = false, oversized = false;
  bool fail() {
    state = State::Failed;
    return false;
  }
  void advance(const uint8_t*& data, uint32_t& len, uint32_t n) {
    data += n;
    len -= n;
    offset += n;
  }
  bool skip(const uint8_t*& data, uint32_t& len) {
    const auto n = std::min(len, padding);
    advance(data, len, n);
    padding -= n;
    return padding == 0;
  }
  bool number(uint32_t start, uint32_t count, uint32_t& result) const {
    uint32_t value = 0;
    const uint32_t radix = aligned ? 16 : 8;
    for (uint32_t i = 0; i < count; ++i) {
      const auto c = header[start + i];
      const uint32_t digit = c >= '0' && c <= '9'   ? c - '0'
                             : c >= 'a' && c <= 'f' ? c - 'a' + 10
                             : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                    : 255;
      if (digit >= radix || value > (UINT32_MAX - digit) / radix) return false;
      value = value * radix + digit;
    }
    result = value;
    return true;
  }
  bool endFile() {
    if (regular) {
      if (!sink.end(sink.ctx)) return fail();
      ++files;
    }
    padding = aligned ? (4 - (offset & 3)) & 3 : 0;
    state = State::DataPadding;
    return true;
  }
};
