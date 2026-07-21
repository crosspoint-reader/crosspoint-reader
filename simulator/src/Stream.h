#pragma once
#include "Print.h"
#include "WString.h"
class Stream : public Print {
public:
  virtual ~Stream() = default;
  virtual int available() = 0;
  virtual int read() = 0;
  virtual int peek() = 0;
  virtual void flush() override {}
  String readStringUntil(char terminator) { return String(""); }
  size_t readBytes(uint8_t *buffer, size_t length) {
    return readBytes(reinterpret_cast<char *>(buffer), length);
  }
  size_t readBytes(char *buffer, size_t length) {
    if (!buffer)
      return 0;

    size_t count = 0;
    while (count < length && available() > 0) {
      const int value = read();
      if (value < 0)
        break;
      buffer[count++] = static_cast<char>(value);
    }
    return count;
  }
};
