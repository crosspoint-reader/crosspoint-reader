#pragma once
#include "Print.h"
class HWCDC : public Print {
 public:
  void begin(unsigned long) {}
  void printf(const char*, ...) {}
  void flush() override {}
  operator bool() const { return true; }
};
extern HWCDC Serial;
