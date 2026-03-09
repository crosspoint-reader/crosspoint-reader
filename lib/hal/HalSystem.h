#pragma once

#include <cstdint>
#include <string>

namespace HalSystem {

void begin();


// Dump panic info to SD card if necessary
void checkPanic();
void clearPanic();

std::string getPanicInfo(bool full = false);
bool isRebootFromPanic();
}  // namespace HalSystem
