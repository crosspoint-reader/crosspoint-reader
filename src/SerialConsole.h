#pragma once

// USB serial control console: reads one "CMD:" line per call from the log
// serial and carries it out (grammar in util/SerialCommand.h). Every command
// except SCREENSHOT answers with one line, "CMD_OK <NAME> ..." or
// "CMD_ERR <NAME> <reason>", so scripts/device_control.py can drive the reader
// for tests and measurements. GET's data follows its CMD_OK, PUT's comes
// before it; see util/SerialTransfer.h. Call from the main loop between
// mappedInputManager.update() and activityManager.loop(), so an injected click
// reaches the screen in the same frame. Returns true when it ran a command, which
// counts as activity for auto-sleep.
bool pollSerialConsole();

struct SerialCommand;

namespace serialconsole {
// The answer line of every command, "CMD_OK <NAME> [detail]" or "CMD_ERR <NAME> <reason>".
void replyOk(const SerialCommand& cmd, const char* detail = "");
void replyErr(const SerialCommand& cmd, const char* reason);
// One protocol line, sent again if HWCDC refused it because another task held the port.
void sendLine(const char* format, ...) __attribute__((format(printf, 1, 2)));
}  // namespace serialconsole
