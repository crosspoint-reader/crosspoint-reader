#pragma once
// Host-portable Arduino String polyfill used by host tests.
// Wraps std::string and exposes the subset of the Arduino String API
// that is needed by PathUtils and CrossPointSettings under test.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

class String {
 public:
  String() = default;
  String(const char* s) : s_(s ? s : "") {}
  String(char c) : s_(1, c) {}
  String(int n) : s_(std::to_string(n)) {}
  String(const std::string& s) : s_(s) {}

  // Assignment
  String& operator=(const char* s) {
    s_ = s ? s : "";
    return *this;
  }
  String& operator=(char c) {
    s_ = std::string(1, c);
    return *this;
  }

  // Concatenation
  String operator+(const String& rhs) const { return String(s_ + rhs.s_); }
  String operator+(const char* rhs) const { return String(s_ + (rhs ? rhs : "")); }
  String operator+(char c) const { return String(s_ + c); }
  friend String operator+(const char* lhs, const String& rhs) { return String(std::string(lhs ? lhs : "") + rhs.s_); }
  String& operator+=(const String& rhs) {
    s_ += rhs.s_;
    return *this;
  }
  String& operator+=(const char* rhs) {
    if (rhs) s_ += rhs;
    return *this;
  }
  String& operator+=(char c) {
    s_ += c;
    return *this;
  }

  // Comparison
  bool operator==(const String& rhs) const { return s_ == rhs.s_; }
  bool operator==(const char* rhs) const { return s_ == (rhs ? rhs : ""); }
  bool operator!=(const String& rhs) const { return !(*this == rhs); }
  bool operator!=(const char* rhs) const { return !(*this == rhs); }
  bool equalsIgnoreCase(const char* rhs) const {
    if (!rhs) return false;
    if (s_.size() != std::strlen(rhs)) return false;
    for (size_t i = 0; i < s_.size(); ++i) {
      if (std::tolower(static_cast<unsigned char>(s_[i])) != std::tolower(static_cast<unsigned char>(rhs[i]))) {
        return false;
      }
    }
    return true;
  }
  bool equalsIgnoreCase(const String& rhs) const { return equalsIgnoreCase(rhs.c_str()); }

  // Element access
  char operator[](size_t i) const { return s_[i]; }

  // Query
  bool isEmpty() const { return s_.empty(); }
  size_t length() const { return s_.size(); }
  const char* c_str() const { return s_.c_str(); }

  // Search — Arduino returns int (-1 = not found)
  int indexOf(const char* sub) const {
    if (!sub) return -1;
    auto pos = s_.find(sub);
    return pos == std::string::npos ? -1 : static_cast<int>(pos);
  }
  int indexOf(const String& sub) const { return indexOf(sub.c_str()); }
  int indexOf(char c) const {
    auto pos = s_.find(c);
    return pos == std::string::npos ? -1 : static_cast<int>(pos);
  }
  int indexOf(char c, int fromIndex) const {
    if (fromIndex < 0 || static_cast<size_t>(fromIndex) >= s_.size()) return -1;
    auto pos = s_.find(c, static_cast<size_t>(fromIndex));
    return pos == std::string::npos ? -1 : static_cast<int>(pos);
  }

  // Predicates
  bool startsWith(const char* prefix) const {
    if (!prefix) return false;
    return s_.rfind(prefix, 0) == 0;
  }
  bool startsWith(const String& prefix) const { return startsWith(prefix.c_str()); }
  bool endsWith(const char* suffix) const {
    if (!suffix) return false;
    const size_t sl = std::strlen(suffix);
    return s_.size() >= sl && s_.compare(s_.size() - sl, sl, suffix) == 0;
  }
  bool endsWith(const String& suffix) const { return endsWith(suffix.c_str()); }

  // Mutation
  void toLowerCase() {
    std::transform(s_.begin(), s_.end(), s_.begin(), [](unsigned char c) { return std::tolower(c); });
  }
  void toUpperCase() {
    std::transform(s_.begin(), s_.end(), s_.begin(), [](unsigned char c) { return std::toupper(c); });
  }
  void replace(const char* from, const char* to) {
    if (!from || !to) return;
    const size_t fl = std::strlen(from);
    const size_t tl = std::strlen(to);
    if (fl == 0) return;
    size_t pos = 0;
    while ((pos = s_.find(from, pos)) != std::string::npos) {
      s_.replace(pos, fl, to);
      pos += tl;
    }
  }
  void replace(const String& from, const String& to) { replace(from.c_str(), to.c_str()); }
  void trim() {
    auto start = s_.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
      s_.clear();
      return;
    }
    auto end = s_.find_last_not_of(" \t\r\n");
    s_ = s_.substr(start, end - start + 1);
  }

  // Substring
  String substring(size_t from) const { return from >= s_.size() ? String("") : String(s_.substr(from)); }
  String substring(size_t from, size_t to) const {
    if (from >= s_.size()) return String("");
    to = std::min(to, s_.size());
    return String(s_.substr(from, to - from));
  }

  // Capacity hint — no-op on host
  void reserve(size_t) {}

  // ArduinoJson integration
  size_t write(uint8_t c) {
    s_.push_back(static_cast<char>(c));
    return 1;
  }
  size_t write(const uint8_t* data, size_t len) {
    if (!data || len == 0) return 0;
    s_.append(reinterpret_cast<const char*>(data), len);
    return len;
  }

  // Conversion
  std::string toStdString() const { return s_; }

 private:
  std::string s_;
};
