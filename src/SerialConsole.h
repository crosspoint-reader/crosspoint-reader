#pragma once

// USB serial control console: reads one "CMD:" line per call from the log
// serial and carries it out (grammar in util/SerialCommand.h). Every command
// except SCREENSHOT answers with one line, "CMD_OK <NAME> ..." or
// "CMD_ERR <NAME> <reason>", so scripts/device_control.py can drive the reader
// for tests and measurements. Call from the main loop between
// mappedInputManager.update() and activityManager.loop(), so an injected click
// reaches the screen in the same frame.
//
// Everything but SCREENSHOT needs ENABLE_SERIAL_CONTROL, which only development
// environments set; release builds answer the rest with CMD_ERR.
//
// Returns true when it ran a command in a development build, which the main
// loop counts as activity, like a button press. Release builds return false.
bool pollSerialConsole();
