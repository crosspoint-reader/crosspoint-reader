#pragma once

#include <cstddef>
#include <cstdint>

#include "BlePageTurner.h"
#include "RadioPolicy.h"

// The radio as the runtime sees it. One implementation is linked into each build:
// SdkRadio.cpp over the SDK's BleKeyboardHost and a FreeRTOS start task, NoRadio.cpp for
// builds without BLE, test/FakeRadio.cpp for the tests. Plain functions rather than a class:
// there is only ever one radio, and the firmware pays no indirection for the seam.
namespace bleturner::port {

// One button edge read from the report bytes (BleKeyboardHost::RawButtonEvent).
struct RawEdge {
  uint8_t reportId = 0;
  uint8_t byteIndex = 0;
  uint8_t value = 0;
  bool pressed = false;
  uint8_t keycode = 0;
  uint8_t mods = 0;
  bool wasRest = false;
  uint32_t atMs = 0;
  uint32_t code() const {
    return static_cast<uint32_t>(value) | static_cast<uint32_t>(byteIndex) << 8 | static_cast<uint32_t>(reportId) << 16;
  }
};

// One decoded key (BleKeyboardHost::KeyEvent).
struct KeyPress {
  uint8_t keycode = 0;
  uint8_t mods = 0;
  bool pressed = true;
};

bool compiledIn();
bool begin();
// Stops the stack, waiting up to timeoutMs; false while it is still stopping.
bool end(uint32_t timeoutMs);
bool running();
bool stopping();
bool connected();
bool connecting();
struct ConnectionActivity {
  bool busy;
  uint32_t advertisements;
};
ConnectionActivity connectionActivity();
bool scanning();
void poll();
bool popRaw(RawEdge& out);
bool popKey(KeyPress& out);
bool armBondedReconnect(uint8_t policy, const char* priorityAddr);
Peer linked();
// durationMs 0 stops a scan.
void scan(uint32_t durationMs);
bool connect(const char* addr);
void disconnect();
void forget(const char* addr);
bool takeConnectFailure(char* out, size_t outLen);
uint8_t bondCount();
Peer bond(uint8_t index);
uint8_t foundCount();
Peer found(uint8_t index);

// Runs detail::startTask() on a task of its own. False when no task could be made.
bool spawnStart();
uint32_t nowMs();
void sleepMs(uint32_t ms);
// Survives a restart (RTC memory on the device).
Memo& restartMemo();

#ifdef BLE_PAGE_TURNER_PROBE
void inject(const uint8_t* frame, size_t len);
unsigned rawOverflows();
#endif
#ifdef BLE_PAGE_TURNER_STACK_PROBE
uint32_t startStackLeft();
#endif

}  // namespace bleturner::port

namespace bleturner::detail {
// The body of one radio start, run by the port on the start task.
void startTask();
}  // namespace bleturner::detail
