// RadioPort for builds without BLE: nothing ever runs, every start is refused.
#if !(defined(FREEINK_CAP_BLE_HID_HOST) && FREEINK_CAP_BLE_HID_HOST)

#include "RadioPort.h"

namespace bleturner::port {
namespace {
Memo memo{0};
}  // namespace

bool compiledIn() { return false; }
bool begin() { return false; }
bool end(uint32_t) { return true; }
bool running() { return false; }
bool stopping() { return false; }
bool connected() { return false; }
bool connecting() { return false; }
bool scanning() { return false; }
void poll() {}
bool popRaw(RawEdge&) { return false; }
bool popKey(KeyPress&) { return false; }
bool armReconnect(const char*) { return false; }
Peer linked() { return {"", ""}; }
void scan(uint32_t) {}
bool connect(const char*) { return false; }
void disconnect() {}
void forget(const char*) {}
bool takeConnectFailure(char*, size_t) { return false; }
uint8_t bondCount() { return 0; }
Peer bond(uint8_t) { return {"", ""}; }
uint8_t foundCount() { return 0; }
Peer found(uint8_t) { return {"", ""}; }
bool spawnStart() { return false; }
uint32_t nowMs() { return 0; }
void sleepMs(uint32_t) {}
Memo& restartMemo() { return memo; }

#ifdef BLE_PAGE_TURNER_PROBE
void inject(const uint8_t*, size_t) {}
unsigned rawOverflows() { return 0; }
#endif
#ifdef BLE_PAGE_TURNER_STACK_PROBE
uint32_t startStackLeft() { return 0; }
#endif

}  // namespace bleturner::port

#endif
