#pragma once

#if ENABLE_SERIAL_CONTROL

class GfxRenderer;

// The serial test API: a host (a person at a terminal, a script, a coding agent) drives the reader
// with one "CMD:..." line per command over the USB serial port and reads one reply line back.
// docs/contributing/serial-test-api.md is the reference; scripts/devctl.py is a host driver.
namespace SerialTestApi {

// Reads what has arrived on the serial port and runs one complete "CMD:" line. Other lines are
// ignored. True when a command ran: the loop counts that as user activity.
bool poll(GfxRenderer& renderer);

// Once per main-loop pass, after ActivityManager::loop(): plays out queued presses and answers a
// pending CMD:IDLE. True while it had such work: the loop counts that as user activity too, so the
// device stays at full speed and awake until a press is played out or a wait is answered.
bool service();

}  // namespace SerialTestApi

#endif
