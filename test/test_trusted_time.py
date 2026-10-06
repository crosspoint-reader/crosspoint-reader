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
struct Preferences {
  bool begin(const char*, bool) { return false; }
  int64_t getLong64(const char*, int64_t value) { return value; }
  void putLong64(const char*, int64_t) {}
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
#include <cassert>
#include <cstring>
int main() {
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
    print("TrustedTime timezone checks passed")
