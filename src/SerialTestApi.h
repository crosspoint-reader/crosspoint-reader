#pragma once

#include <cstdint>
#include <string_view>

// The serial test interface: a host (a person at a terminal, a script, an AI agent) drives the
// device with one "CMD:..." line per command over the USB serial port and reads back one reply
// line with a stable prefix. docs/serial-test-api.md is the reference for every command, its
// replies and its use; tools/devctl.py is a host driver.
//
// The commands go through the same routes the UI does: a press is HalGPIO::injectPress, an
// activity is an ActivityManager::goTo*(), a setting is the row the web settings API writes. The
// argument grammar lives in util/SerialCommandGrammar so it can be tested on the host.
//
// SERIAL_TEST_API (set by the development environments in platformio.ini) builds the full set.
// Without it only the original commands remain -- BTN (the six front and side keys, :LONG), ACT
// (HOME, READER:<path>, SETTINGS, BOOKMARKS, FILES, LIBRARY), STATE (screen, stack, the reader's
// position), SCREENSHOT and BOOTLOG -- and every probe below compiles to nothing.
#ifndef SERIAL_TEST_API
#define SERIAL_TEST_API 0
#endif

class GfxRenderer;

namespace SerialTestApi {

// One received line. Anything not starting with "CMD:" is ignored, so log noise and a terminal's
// stray keys do no harm. Called by the main loop, and by the USB transfer screen, which owns the
// port while it is up and reads the lines itself. Replies and event lines are only ever written
// from the loop task, so they never interleave with each other or a screenshot.
void handleLine(std::string_view line);

// ScreenshotUtil's host sender (CMD:SCREENSHOT): SCREENSHOT_START:<w>:<h>:<bytes>, the raw
// 1-bit frame, SCREENSHOT_END; or SCREENSHOT_ERROR:<why>.
void sendScreenshot(const uint8_t* frame, uint32_t size, int width, int height, bool busy);

// Set by CMD:SLEEP and CMD:REBOOT; main.cpp acts on them after the reply is out, since sleep and
// the restart helpers live there.
enum class PowerRequest : uint8_t { None, Sleep, Reboot };

#if SERIAL_TEST_API
// Once from setup(): the renderer CMD:TOUCH maps screen coordinates with.
void begin(GfxRenderer& renderer);

// Once per main loop pass, after the activity's loop(): answers a pending CMD:IDLE and writes the
// EVT: lines a host asked for (CMD:EVENTS:ON).
void service();
PowerRequest takePowerRequest();

// Probes the UI calls from whichever task draws. Each copies a few bytes into a fixed slot (no
// heap) for CMD:STATE and the EVT: lines to report later.
// A popup with this text was just drawn (BaseTheme/LyraTheme drawPopup).
void notePopup(const char* text);
// A list or menu was drawn with row `index` highlighted, showing `label` (null when unknown). The
// last list drawn on the current screen wins.
void noteSelection(int index, const char* label);
// A section build finished: `spineIndex` is 0-based, as the reader holds it.
void noteBuildDone(int spineIndex, int pageCount);
#else
inline void begin(GfxRenderer&) {}
inline void service() {}
inline PowerRequest takePowerRequest() { return PowerRequest::None; }
inline void notePopup(const char*) {}
inline void noteSelection(int, const char*) {}
inline void noteBuildDone(int, int) {}
#endif

}  // namespace SerialTestApi
