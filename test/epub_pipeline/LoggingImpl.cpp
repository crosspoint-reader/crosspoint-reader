// Host logging for the pipeline harness: LOG_* goes to stderr so pipeline
// failures are diagnosable, while stdout stays reserved for the canonical dump.
// (Other host tests use test/zip_entry_reader/LoggingStub.cpp, which is silent.)
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>

#include "LogObserver.h"

namespace {
LogObserver g_observer = nullptr;
}  // namespace

void setLogObserver(const LogObserver observer) { g_observer = observer; }

void logPrintf(const char* level, const char* origin, const char* format, ...) {
  char line[1024];
  va_list args;
  va_start(args, format);
  std::vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  if (g_observer != nullptr) g_observer(level, origin, line);
  std::fprintf(stderr, "[%s][%s] %s\n", level, origin, line);
}
std::string getLastLogs() { return {}; }
void clearLastLogs() {}
bool sanitizeLogHead() { return false; }

extern "C" {
uint32_t uzlib_adler32(const void*, unsigned int, uint32_t prev) { return prev; }
uint32_t uzlib_crc32(const void*, unsigned int, uint32_t crc) { return crc; }
}
