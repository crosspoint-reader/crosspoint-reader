#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

unsigned long millis();
void delay(unsigned long milliseconds);
void configTzTime(const char* timezone, const char* server1, const char* server2, const char* server3 = nullptr);
