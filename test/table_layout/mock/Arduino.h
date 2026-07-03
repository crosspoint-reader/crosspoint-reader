#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
// Minimal Arduino shim for host builds of the table-layout unit test.
class String : public std::string {
 public:
  using std::string::string;
  String() = default;
  String(const std::string& s) : std::string(s) {}
  String(const char* s) : std::string(s ? s : "") {}
};
inline unsigned long millis() { return 0; }
inline void delay(unsigned long) {}
#define IRAM_ATTR
#define DRAM_ATTR
