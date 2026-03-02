#pragma once
// Minimal Arduino stub for native unit tests.
// Maps Arduino-specific types to standard C++ equivalents.
#include <cstdint>
#include <string>

using String = std::string;
using byte = uint8_t;

inline void delay(unsigned long) {}
