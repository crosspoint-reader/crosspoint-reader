#pragma once
#include <cstddef>
#include <cstdint>
constexpr int HIGH = 1;
constexpr int LOW = 0;
inline int digitalRead(int) { return HIGH; }
inline void delay(unsigned long) {}
