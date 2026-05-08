#pragma once

#include <chrono>
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#define PROGMEM
#define F(x) x
#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2
#define A0 0
#define RTC_NOINIT_ATTR
#define DRAM_ATTR
#define IRAM_ATTR

class String {
  std::string value;

 public:
  String() = default;
  String(const char* s) : value(s ? s : "") {}
  explicit String(const std::string& s) : value(s) {}
  explicit String(std::string&& s) : value(std::move(s)) {}
  String(char c) : value(1, c) {}
  String(int n) : value(std::to_string(n)) {}
  String(unsigned int n) : value(std::to_string(n)) {}
  String(long n) : value(std::to_string(n)) {}
  String(unsigned long n) : value(std::to_string(n)) {}

  const char* c_str() const { return value.c_str(); }
  bool isEmpty() const { return value.empty(); }
  size_t length() const { return value.length(); }
  void trim() {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
      value.clear();
      return;
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    value = value.substr(first, last - first + 1);
  }
  bool startsWith(const char* prefix) const { return value.rfind(prefix ? prefix : "", 0) == 0; }
  String substring(size_t start) const { return String(start < value.size() ? value.substr(start) : std::string()); }
  String substring(size_t start, size_t end) const {
    if (start >= value.size()) return {};
    return String(value.substr(start, end > start ? end - start : 0));
  }
  int indexOf(char c) const {
    const auto pos = value.find(c);
    return pos == std::string::npos ? -1 : static_cast<int>(pos);
  }
  char charAt(size_t i) const { return i < value.size() ? value[i] : '\0'; }
  void reserve(size_t n) { value.reserve(n); }
  size_t write(uint8_t c) {
    value.push_back(static_cast<char>(c));
    return 1;
  }
  size_t write(const uint8_t* data, size_t n) {
    value.append(reinterpret_cast<const char*>(data), n);
    return n;
  }
  size_t write(const char* data, size_t n) {
    value.append(data, n);
    return n;
  }
  void remove(size_t index) {
    if (index < value.size()) value.erase(index);
  }
  void replace(const char* from, const char* to) {
    const std::string f = from ? from : "";
    const std::string t = to ? to : "";
    if (f.empty()) return;
    size_t pos = 0;
    while ((pos = value.find(f, pos)) != std::string::npos) {
      value.replace(pos, f.size(), t);
      pos += t.size();
    }
  }
  operator const char*() const { return value.c_str(); }
  char operator[](size_t i) const { return value[i]; }
  String& operator+=(const String& other) {
    value += other.value;
    return *this;
  }
  String& operator+=(const char* other) {
    value += other ? other : "";
    return *this;
  }
  String& operator+=(char c) {
    value += c;
    return *this;
  }
  friend String operator+(const String& a, const String& b) { return String(a.value + b.value); }
  friend String operator+(const String& a, const char* b) { return String(a.value + (b ? b : "")); }
  friend String operator+(const char* a, const String& b) { return String((a ? a : "") + b.value); }
  friend bool operator==(const String& a, const String& b) { return a.value == b.value; }
  friend bool operator!=(const String& a, const String& b) { return !(a == b); }
};

class Print {
 public:
  virtual ~Print() = default;
  virtual size_t write(uint8_t b) { return write(&b, 1); }
  virtual size_t write(const uint8_t* buffer, size_t size) {
    return fwrite(buffer, 1, size, stdout);
  }
  size_t write(const char* s) { return write(reinterpret_cast<const uint8_t*>(s), strlen(s)); }
  size_t print(const char* s) { return write(s ? s : ""); }
  size_t print(const String& s) { return print(s.c_str()); }
  size_t println(const char* s = "") {
    size_t n = print(s);
    n += print("\n");
    return n;
  }
  virtual void flush() { fflush(stdout); }
  virtual int available() { return 0; }
  virtual String readStringUntil(char) { return {}; }
  size_t printf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int n = vprintf(format, args);
    va_end(args);
    return n < 0 ? 0 : static_cast<size_t>(n);
  }
};

class HWCDC : public Print {
 public:
  void begin(unsigned long) {}
  operator bool() const { return true; }
};

extern HWCDC Serial;

class ESPClass {
 public:
  uint32_t getFreeHeap() const { return 1024 * 1024 * 64; }
  uint32_t getHeapSize() const { return 1024 * 1024 * 64; }
  uint32_t getMinFreeHeap() const { return 1024 * 1024 * 64; }
  uint32_t getMaxAllocHeap() const { return 1024 * 1024 * 32; }
  void restart() const { std::exit(0); }
};

extern ESPClass ESP;

inline unsigned long millis() {
  static const auto start = std::chrono::steady_clock::now();
  return static_cast<unsigned long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
}
inline unsigned long micros() {
  static const auto start = std::chrono::steady_clock::now();
  return static_cast<unsigned long>(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
}
inline void delay(unsigned long ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
inline void yield() { std::this_thread::yield(); }
inline void pinMode(int, int) {}
inline int digitalRead(int) { return HIGH; }
inline void digitalWrite(int, int) {}
inline int analogRead(int) { return 2048; }
inline int getCpuFrequencyMhz() { return 160; }
inline bool setCpuFrequencyMhz(int) { return true; }
inline long random(long max) { return max <= 0 ? 0 : std::rand() % max; }
inline long random(long min, long max) { return min + random(max - min); }
