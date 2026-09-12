#pragma once

namespace SerialControl {
#ifdef ENABLE_SERIAL_LOG
void beginFrame();
void endFrame();
void poll();
struct Frame {
  Frame() { beginFrame(); }
  ~Frame() { endFrame(); }
};
#else
inline void poll() {}
struct Frame {};
#endif
}  // namespace SerialControl
