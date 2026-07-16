// BleClock — persisted wall-clock floor for the RTC-less X4 (PROTOCOL-v3 §3).
//
// The ESP32-C3 loses time across deep-sleep (deep-sleep == full chip reset), so
// time() reads 0 until a sync feeds it the phone's clock. That made positions
// saved before the first sync of a boot stamp updatedAt=0, which loses
// newest-wins forever. This keeps a monotonic "highest unix time this device has
// ever known" on flash: seed time() from it on boot, advance it on every real
// clock (phone NTP) and every progress save. The phone is the only time source —
// no internet NTP. Zero cost when BLE sync is off (nobody calls these).
//
// Header-only inline (mirrors EpubReaderUtils): avoids an include cycle with
// BleProgressBridge, which includes EpubReaderUtils.
#pragma once

#include <HalStorage.h>
#include <sys/time.h>

#include <cstdint>
#include <ctime>

namespace BleClock {

constexpr char kFloorPath[] = "/.crosspoint/ble-clock.bin";
constexpr int64_t kValidFrom = 1000000000;  // 2001-09-09; below this = "clock unset"

inline int64_t readFloor() {
  HalFile f;
  if (!Storage.openFileForRead("BleClk", kFloorPath, f)) return 0;
  uint8_t d[8] = {0};
  if (f.read(d, sizeof(d)) < 8) return 0;
  int64_t t = 0;
  for (int i = 0; i < 8; i++) t |= (static_cast<int64_t>(d[i]) << (8 * i));
  return t;
}

// Raise the floor to `t` (monotonic non-decreasing). No-op for an unset/older t.
inline void writeFloor(int64_t t) {
  if (t <= kValidFrom) return;
  if (t <= readFloor()) return;
  uint8_t d[8];
  for (int i = 0; i < 8; i++) d[i] = static_cast<uint8_t>((static_cast<uint64_t>(t) >> (8 * i)) & 0xFF);
  HalFile f;
  if (Storage.openFileForWrite("BleClk", kFloorPath, f)) {
    f.write(d, sizeof(d));
    f.flush();
  }
}

// Seed the system clock from the floor when it is unset (post-deep-sleep boot).
// After this, edits stamp floor + uptime — real-ish and never 0.
inline void seedFromFloor() {
  const int64_t f = readFloor();
  if (f > kValidFrom && static_cast<int64_t>(time(nullptr)) < f) {
    struct timeval tv = {static_cast<time_t>(f), 0};
    settimeofday(&tv, nullptr);
  }
}

}  // namespace BleClock
