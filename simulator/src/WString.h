#pragma once

// ArduinoJson auto-detects Arduino's String class via the ARDUINO macro, which
// is not defined in the simulator's native build. Enable the support manually
// so .as<String>() and serialize-to-String resolve to ArduinoJson's built-in
// converter (matching firmware behaviour) instead of the std::string-only path.
#ifndef ARDUINOJSON_ENABLE_ARDUINO_STRING
#define ARDUINOJSON_ENABLE_ARDUINO_STRING 1
#endif

#include <cstdint>
#include <cstring>
#include <string>

class String {
public:
  std::string s;
  String() {}
  String(const char *str) : s(str ? str : "") {}
  explicit String(const std::string &str) : s(str) {}
  String(uint16_t num) : s(std::to_string(num)) {}

  String &operator=(const char *str) {
    s = str ? str : "";
    return *this;
  }
  String &operator=(const std::string &str) {
    s = str;
    return *this;
  }
  String &operator+=(const char *str) {
    s += str;
    return *this;
  }
  String &operator+=(char c) {
    s += c;
    return *this;
  }
  String &operator+=(const String &other) {
    s += other.s;
    return *this;
  }

  bool startsWith(const String &prefix) const { return s.find(prefix.s) == 0; }
  bool startsWith(const char *prefix) const { return s.find(prefix) == 0; }
  String substring(unsigned int from,
                   unsigned int to = (unsigned int)-1) const {
    if (from >= s.length())
      return String();
    return String(
        s.substr(from, to == (unsigned int)-1 ? std::string::npos : to - from));
  }
  int lastIndexOf(char c) const { return s.rfind(c); }
  int indexOf(char c) const { return s.find(c); }
  int indexOf(char c, size_t pos) const { return s.find(c, pos); }
  int indexOf(const char *substr) const { return s.find(substr); }
  int indexOf(const char *substr, size_t pos) const {
    return s.find(substr, pos);
  }
  int indexOf(const String &substr) const { return s.find(substr.s); }
  int indexOf(const String &substr, size_t pos) const {
    return s.find(substr.s, pos);
  }
  char charAt(size_t index) const {
    return index < s.length() ? s[index] : '\0';
  }
  long toInt() const { return std::stoi(s); }
  int read() const {
    if (readPos < s.length())
      return s[readPos++];
    return -1;
  }
  bool endsWith(const String &suffix) const {
    return s.size() >= suffix.s.size() &&
           s.substr(s.size() - suffix.s.size()) == suffix.s;
  }
  bool endsWith(const char *suffix) const {
    return suffix && s.size() >= strlen(suffix) &&
           s.substr(s.size() - strlen(suffix)) == suffix;
  }
  bool equals(const String &other) const { return s == other.s; }
  bool equals(const char *other) const { return s == (other ? other : ""); }
  void trim() {
    size_t first = s.find_first_not_of(" \t\n\r");
    if (first == std::string::npos) {
      s.clear();
      return;
    }
    size_t last = s.find_last_not_of(" \t\n\r");
    s = s.substr(first, (last - first + 1));
  }
  void replace(char find, char replaceWith) {
    for (char &c : s) {
      if (c == find)
        c = replaceWith;
    }
  }
  void replace(const char *find, const char *replaceWith) {
    if (!find || !*find)
      return;
    const std::string f(find);
    const std::string r(replaceWith ? replaceWith : "");
    size_t pos = 0;
    while ((pos = s.find(f, pos)) != std::string::npos) {
      s.replace(pos, f.size(), r);
      pos += r.size();
    }
  }
  void replace(const String &find, const String &replaceWith) {
    replace(find.s.c_str(), replaceWith.s.c_str());
  }
  bool isEmpty() const { return s.empty(); }
  size_t length() const { return s.length(); }
  const char *c_str() const { return s.c_str(); }
  bool operator==(const char *other) const { return s == (other ? other : ""); }
  bool operator!=(const char *other) const { return !(*this == other); }
  bool operator==(const String &other) const { return s == other.s; }
  bool operator!=(const String &other) const { return s != other.s; }

  size_t write(uint8_t c) {
    s.push_back(c);
    return 1;
  }
  size_t write(const uint8_t *buffer, size_t size) {
    s.append((const char *)buffer, size);
    return size;
  }
  size_t write(const char *buffer, size_t size) {
    s.append(buffer, size);
    return size;
  }

  // ArduinoJson's writer for ::String calls concat() to append chunks.
  unsigned char concat(const char *str) {
    if (!str)
      return 0;
    s += str;
    return 1;
  }
  unsigned char concat(const char *str, unsigned int length) {
    if (!str)
      return 0;
    s.append(str, length);
    return 1;
  }
  unsigned char concat(const String &other) {
    s += other.s;
    return 1;
  }

  operator const char *() const { return c_str(); }

private:
  mutable size_t readPos = 0;
};

inline String operator+(const String &lhs, const String &rhs) {
  String res(lhs);
  res += rhs.c_str();
  return res;
}

inline String operator+(const String &lhs, const char *rhs) {
  String res(lhs);
  res += rhs;
  return res;
}

inline String operator+(const char *lhs, const String &rhs) {
  String res(lhs);
  res += rhs.c_str();
  return res;
}
