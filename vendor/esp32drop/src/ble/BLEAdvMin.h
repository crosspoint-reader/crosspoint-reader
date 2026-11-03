// BLEAdvMin — advertise, and nothing else.
//
// A BLE broadcaster for ESP32-C3/S3 built out of four standard HCI commands sent over
// Espressif's own VHCI transport. There is no host stack: no GAP, no GATT, no L2CAP, no
// security manager, no connection state. The device transmits ADV_NONCONN_IND and never
// listens.
//
// WHY THIS EXISTS. The platform's BLE entry point (NimBLE, or Bluedroid on ESP32) brings
// up a full dual-role host. Measured on an M5Stack StopWatch (ESP32-S3), NimBLE costs
// 71,764 B of internal RAM. Half of that is the host, and a beacon uses none of it.
// This path costs 26,620 B measured, for identical observable behaviour.
//
// WHAT THIS IS NOT. It is not a from-scratch BLE implementation. The link layer is
// Espressif's precompiled controller (libble_app.a) — the same one NimBLE drives. What is
// skipped is only the host library above it. Going below ~26 KB means either rebuilding
// the IDF controller or programming the BLE MAC registers directly, which Espressif does
// not document, so 26 KB is the floor and there is nothing left to trim.
//
// The controller's cost was measured twice, in two trees, and agrees: 26,452 B standalone,
// 26,620 B here. ad_cycle.cpp uses that: it reads the heap after starting the advertiser
// and treats a controller that took no memory as a failure, because begin() and enable()
// both return OK whether or not the radio actually came up.
//
// end() gives it all back. Twenty start/stop cycles leave 248 B in 3 blocks and return the
// largest free block exactly to its baseline -- so the advertiser can be cycled against
// AWDL, which is what ad_cycle.cpp does, without fragmenting the arena TLS needs.

#pragma once
/* Part of ESP32Drop, under the same 0BSD terms as the rest of it. The implementation
   compiles only when ESP32DROP_SENDER is defined -- see BLEAdvMin.cpp for why that matters
   to a sketch that only receives. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Return codes. Anything non-zero is an esp_err_t from the controller, except the
// negative values below, which are this library's own.
enum {
  BLEADV_OK = 0,
  BLEADV_E_ALREADY = -1,      // begin() called twice
  BLEADV_E_NOT_STARTED = -2,  // called before a successful begin()
  BLEADV_E_TOOLONG = -3,      // advertising data longer than 31 bytes
  BLEADV_E_VHCI_BUSY = -4,    // controller never reported itself ready to accept a command
};

typedef struct {
  // Advertising interval, milliseconds. The controller picks a random 0–10 ms delay on
  // top of this, per spec. Range 20–10240; the default matches what the AirDrop wake
  // probe used.
  uint16_t interval_ms;

  // Controller task stack, bytes. The vendor default is 4096. Lowering it is the last
  // documented knob that still moves internal RAM; if the controller cannot start with
  // the value given, begin() returns the error rather than hanging.
  uint16_t task_stack;

  // Place the controller's code in flash rather than internal RAM. Vendor default is
  // false. UNMEASURED in this tree — flip it and read bleadv_ram_cost().
  bool run_in_flash;

  // TX power, esp_power_level_t (0 = -27 dBm … 15 = +20 dBm on S3). 0xFF leaves the
  // vendor default (+9 dBm). No RAM effect; here because a wake signal that is too loud
  // is a different bug from one that is too quiet.
  uint8_t tx_power;
} bleadv_config_t;

// Vendor defaults, with everything a broadcaster does not do switched off. A function
// rather than a designated-initialiser macro: this header is included from .cpp, and
// compound literals there are a compiler extension rather than a guarantee.
static inline bleadv_config_t bleadv_config_default(void) {
  bleadv_config_t c;
  c.interval_ms = 30;
  c.task_stack = 4096;
  c.run_in_flash = false;
  c.tx_power = 0xFF;
  return c;
}

// Bring up the controller and load the advertising payload. Does NOT start transmitting;
// call bleadv_enable(true) for that, so the caller controls exactly when the radio opens.
//
// `adv` is the raw AD-structure payload — length/type/value triples, as they appear on
// air, WITHOUT the leading total-length byte, which this adds. Max 31 bytes.
//
// ⚠️ MUST be called before Wi-Fi/AWDL is brought up if internal RAM is tight: this needs
//    ~27 KB contiguous, and it is not there once a Wi-Fi stack is resident.
// ⚠️ Requires the btInUse() override, which BLEAdvMin.cpp provides. See the note there.
int bleadv_begin(const uint8_t* adv, size_t len, const bleadv_config_t* cfg);

// Replace the payload. Safe while advertising; the controller swaps it between events.
int bleadv_set_data(const uint8_t* adv, size_t len);

// Start or stop transmitting. This is the only call in the hot path — one HCI command,
// no allocation — so it is cheap enough to toggle on a duty cycle.
int bleadv_enable(bool on);

// Set the advertiser address to a random static address. Call before bleadv_enable().
// `addr` is little-endian, 6 bytes; the top two bits of addr[5] are forced to 1 as the
// spec requires. Without this the public factory BD_ADDR is used, which is a stable
// identifier the device broadcasts to everyone in range.
int bleadv_set_random_address(const uint8_t addr[6]);

// Stop advertising and hand every byte back. Measured recovery on S3: 99.6 % — a ~260 B
// leak remains, and it lands where it can split the heap arena. If a large contiguous
// allocation must follow, do this teardown before anything else has fragmented the heap.
int bleadv_end(void);

// Read the controller's public address (little-endian, 6 bytes) — what a scanner shows
// when no random address has been set. Ground truth for identifying the beacon.
int bleadv_read_bd_addr(uint8_t out[6]);

// ONE-WAY. Hand the BLE controller's static BSS/data back to the heap, on top of what
// end() already returned. After this, BLE cannot be started again until a reboot —
// bleadv_begin() will fail rather than hang.
//
// This exists because the boot-time release that the Arduino core would normally perform
// is suppressed by this library (see BLEAdvMin.cpp). Calling this after end() is how a
// firmware that needs BLE once, early, gives the memory to whatever runs next.
int bleadv_release_forever(void);

// Internal RAM consumed by begin(), in bytes. Zero before begin() / after end().
int bleadv_ram_cost(void);

// Of bleadv_ram_cost(), the share taken by esp_bt_controller_init() alone. The remainder
// belongs to esp_bt_controller_enable(). Diagnostic: it says which call a saving would
// have to come out of.
int bleadv_ram_cost_init(void);

// True between a successful begin() and end().
bool bleadv_active(void);

#ifdef __cplusplus
}
#endif
