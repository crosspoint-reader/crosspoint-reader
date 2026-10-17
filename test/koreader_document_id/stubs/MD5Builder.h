#pragma once
// Recording stand-in for Arduino's MD5Builder. The digest is not MD5: a document id is a pure
// function of the bytes hashed, so the tests check exactly which bytes were fed, in which order.
// toString() still gives 32 hex characters, the shape the hash cache validates on load.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "WString.h"

namespace kodoc_fixture {

struct HashLog {
  std::vector<uint8_t> bytes;  // everything add() was given, in order
  size_t largestAdd = 0;       // a failed read cast to size_t shows up here as SIZE_MAX
};

inline HashLog hashLog;

}  // namespace kodoc_fixture

class MD5Builder {
 public:
  void begin() { kodoc_fixture::hashLog.bytes.clear(); }
  void add(const uint8_t* data, size_t len) {
    auto& log = kodoc_fixture::hashLog;
    if (len > log.largestAdd) log.largestAdd = len;
    if (len > 1024 * 1024) return;  // never a real sample: record it, do not read it
    log.bytes.insert(log.bytes.end(), data, data + len);
  }
  void add(const char* s) { add(reinterpret_cast<const uint8_t*>(s), std::strlen(s)); }
  void calculate() {}
  String toString() const {
    uint64_t a = 1469598103934665603ull, b = 0x9E3779B97F4A7C15ull;
    for (const uint8_t byte : kodoc_fixture::hashLog.bytes) {
      a = (a ^ byte) * 1099511628211ull;
      b = (b ^ byte) * 0x100000001B3ull + 1;
    }
    char hex[33];
    std::snprintf(hex, sizeof(hex), "%016llx%016llx", static_cast<unsigned long long>(a),
                  static_cast<unsigned long long>(b));
    return String(hex);
  }
};
