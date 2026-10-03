#pragma once

#if defined(ENABLE_SERIAL_CONTROL) && !defined(ENABLE_SERIAL_LOG)
#error "ENABLE_SERIAL_CONTROL requires ENABLE_SERIAL_LOG"
#endif

// Serial-log builds accept CMD:SCREENSHOT. Development builds with
// ENABLE_SERIAL_CONTROL also accept input injection and state queries.
namespace SerialControl {
#ifdef ENABLE_SERIAL_LOG
void poll();
#else
inline void poll() {}
#endif
#ifdef ENABLE_SERIAL_CONTROL
void beginFrame();
void endFrame();
struct Frame {
  Frame() { beginFrame(); }
  ~Frame() { endFrame(); }
};
#else
struct Frame {};
#endif
}  // namespace SerialControl
