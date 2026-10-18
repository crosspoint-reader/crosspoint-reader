// Stubs for device-only symbols referenced by GifToFramebufferConverter.cpp.
// This translation unit is only compiled for host unit tests.
#include <cstdint>
#include <string>

// Logging
void logPrintf(const char*, const char*, const char*, ...) {}
std::string getLastLogs() { return {}; }
void clearLastLogs() {}
bool sanitizeLogHead() { return false; }

// GIF decoder only uses ESP.getFreeHeap() inside decodeToFramebuffer(), which
// is not called by decodeFirstFrameToGrayscale(). Provide a stub anyway so the
// translation unit links cleanly.
struct EspClass {
  static uint32_t getFreeHeap() { return 256u * 1024u; }
};
EspClass ESP;

// WDT reset — called from decodeToFramebuffer() only, not from the test path.
extern "C" void esp_task_wdt_reset() {}

// ImageBlock.cpp's (the reader's memory ladder counts decodes short of memory); not linked here.
namespace image_scratch {
void noteHeapShort() {}
}  // namespace image_scratch
