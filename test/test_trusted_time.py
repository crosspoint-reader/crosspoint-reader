"""Compile TrustedTime against host stubs and check its timezone contract."""

from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]

STUBS = {
    "Arduino.h": r'''
#pragma once
#include <cstdlib>
#include <ctime>
#include <sys/time.h>
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
inline bool enabled = false;
inline bool completed = true;
inline int configurations = 0;
inline unsigned long ticks = 0;
inline unsigned long millis() { return ticks; }
inline void delay(unsigned long ms) { ticks += ms; }
inline void configTzTime(const char* tz, const char*) {
  enabled = true;
  ++configurations;
  setenv("TZ", tz, 1);
  tzset();
}
''',
    "Preferences.h": r'''
#pragma once
#include <cstdint>
// Stands in for the NVS floor: keeps the last written value for the process so
// init()'s restore and adopt()'s wear guard are observable.
inline int64_t storedFloor = 0;
struct Preferences {
  bool begin(const char*, bool) { return true; }
  int64_t getLong64(const char*, int64_t value) { return value ? value : storedFloor; }
  void putLong64(const char*, int64_t v) { storedFloor = v; }
  void end() {}
};
''',
    "Logging.h": '#pragma once\n#define LOG_DBG(...) ((void)0)\n',
    "esp_sntp.h": r'''
#pragma once
#include <Arduino.h>
constexpr int SNTP_SYNC_STATUS_COMPLETED = 1;
inline bool esp_sntp_enabled() { return enabled; }
inline int sntp_get_sync_status() { return completed ? SNTP_SYNC_STATUS_COMPLETED : 0; }
inline void sntp_set_time_sync_notification_cb(void (*)(timeval*)) {}
''',
}

CHECK = r'''
#include <TrustedTime.h>
#include <Arduino.h>
#include <Preferences.h>
#include <cassert>
#include <cstring>
int main() {
  // adopt(): the RTC baseline. Runs before the SNTP checks so the in-RAM floor
  // still starts at 0. Asserted on the floor, not on time(): settimeofday()
  // cannot move the clock of whatever host runs this check. Baselines come from
  // the host clock rather than fixed dates so the checks stay valid on any date,
  // and trustedNow() stays out of the sequence because it raises the floor to
  // the host clock, which would decide these adopts instead of the epoch under
  // test.
  storedFloor = 0;
  // A device with no RTC contributes no epoch, and an unset chip answers
  // 2000-01-01; neither may install a clock.
  trustedtime::adopt(0);
  trustedtime::adopt(946684800);
  assert(storedFloor == 0);

  // Deep-sleep wake: the system clock is live while the floor is still empty,
  // so a stale RTC must not rewind it. The host clock stands in for the one
  // deep sleep preserves, so it must be a plausible date with a day's room on
  // each side for the stale epoch and for the clock ticking under the read.
  const int64_t live = static_cast<int64_t>(time(nullptr));
  if (live >= 1735689600LL + 86400 && live <= 4102444800LL - 86400) {
    trustedtime::adopt(live - 86400);
    const int64_t after = static_cast<int64_t>(time(nullptr));
    // The clock adopt() actually read is somewhere in that window, so the floor
    // it wrote has to land in it too. A stale RTC would write live - 86400.
    if (after >= 1735689600LL && after <= 4102444800LL) {
      assert(storedFloor >= live && storedFloor <= after);
    }
  }

  // Ahead of any clock the host has, so the adopts below are decided by the
  // epoch under test; floored so they still mean something on a host whose own
  // clock is not a plausible date.
  const int64_t base = live > 1735689600LL ? live : 1735689600LL;
  const int64_t rtcNow = base + 3600;
  trustedtime::adopt(rtcNow);
  assert(storedFloor == rtcNow);

  // A lagging RTC must not move the floor: adopt() writes it only when the
  // epoch moves forward.
  trustedtime::adopt(rtcNow - 86400);
  assert(storedFloor == rtcNow);

  // A chip whose calendar runs a year past the floor we already trust is
  // faulty, not right. Still inside the 2100 window, so this is the lead check
  // and not the range check.
  trustedtime::adopt(rtcNow + 400 * 86400);
  assert(storedFloor == rtcNow);

  // Past the supported window (2100) a value is a fault, not a date, and the
  // floor never lowers again -- adopting it would lock out every real clock.
  trustedtime::adopt(4102444801);
  assert(storedFloor == rtcNow);

  trustedtime::adopt(rtcNow + 3600);
  assert(storedFloor == rtcNow + 3600);

  // init()'s floor restore must leave an adopted floor alone, and a clock the
  // floor already trusts is reported rather than pulled back to the host date.
  trustedtime::init();
  assert(trustedtime::trustedNow() >= rtcNow + 3600);
  assert(storedFloor == rtcNow + 3600);

  const char* zone = "EST5EDT,M3.2.0,M11.1.0";
  setenv("TZ", zone, 1);
  tzset();
  trustedtime::startSync();
  assert(configurations == 1 && strcmp(getenv("TZ"), zone) == 0);
  trustedtime::startSync();
  assert(configurations == 1);
  assert(trustedtime::syncNow(200));
  assert(configurations == 2 && strcmp(getenv("TZ"), zone) == 0);
  completed = false;
  assert(!trustedtime::syncNow(200));
  assert(strcmp(getenv("TZ"), zone) == 0);
  unsetenv("TZ");
  enabled = false;
  trustedtime::startSync();
  assert(strcmp(getenv("TZ"), "UTC0") == 0);
}
'''


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="trusted-time-test-") as directory:
        work = Path(directory)
        for name, content in STUBS.items():
            (work / name).write_text(content)
        (work / "check.cpp").write_text(CHECK)
        subprocess.run([
            "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
            f"-I{work}", f"-I{ROOT / 'lib/TrustedTime'}",
            str(ROOT / "lib/TrustedTime/TrustedTime.cpp"), str(work / "check.cpp"),
            "-o", str(work / "check"),
        ], check=True)
        subprocess.run([str(work / "check")], check=True)
    print("TrustedTime clock and timezone checks passed")
