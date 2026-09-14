#pragma once

#include <cstdint>
#include <cstdio>

inline unsigned long fakeMillis = 0;
inline unsigned long millis() { return fakeMillis; }
inline void delay(unsigned long) {}
inline void configTzTime(const char*, const char*, const char*) {}
