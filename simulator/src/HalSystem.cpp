#include "HalSystem.h"

void HalSystem::begin() {}
void HalSystem::restart() { exit(0); }
void HalSystem::checkPanic() {}
void HalSystem::clearPanic() {}
std::string HalSystem::getPanicInfo(bool full) { return {}; }
bool HalSystem::isRebootFromPanic() { return false; }
