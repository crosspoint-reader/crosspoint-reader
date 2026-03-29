#pragma once

#include <cstdint>
#include <ctime>

/// Lightweight wall-clock facade.
///
/// The ESP32-C3 has no battery-backed RTC, so wall-clock time is lost on every
/// deep-sleep / power cycle.  HalClock bridges this gap using three layers:
///
///  - **LP timer** (`esp_clk_rtc_time()`) — keeps running during deep sleep
///    when `keepClockAlive` is enabled (GPIO13 stays HIGH).  Used to compute
///    elapsed time and correct the stored epoch on wake.
///  - **RTC memory** (`RTC_NOINIT_ATTR`) — survives deep sleep, lost on cold
///    boot.  Stores the epoch + LP timer value captured before sleep.
///  - **NVS** (flash key-value store) — survives power cycles.  Fallback when
///    RTC memory is unavailable (cold boot).
///
/// Usage:
///  1. On boot, call `restore()` to seed the system clock from the best
///     available source (RTC memory + LP correction > NVS).
///  2. After a successful NTP sync, call `syncNtp()`.
///  3. Before entering deep sleep, call `saveBeforeSleep()`.
///
/// `now()` returns the best-effort epoch (0 if never synced).
namespace HalClock {

/// Perform an NTP sync (requires WiFi to be connected).  Starts SNTP,
/// waits up to 5 seconds for completion, then captures the result.
/// Returns true if the sync succeeded.
bool syncNtp();

/// Apply timezone/DST rules via the POSIX TZ string for the given setting.
void applyTimezone(uint8_t timeZoneSetting);

/// Call just before deep sleep.  Snapshots the current system time to RTC
/// memory and NVS so it can be restored on wake / cold boot.  Pass true when
/// the LP timer is kept alive during sleep.
void saveBeforeSleep(bool keepLpAlive);

/// Call on boot to seed the system clock from the best available stored
/// value.  When RTC memory is valid (deep-sleep wake) and the LP timer was
/// running, the restored time includes elapsed-time correction.  Falls back
/// to NVS for cold boot (stale, but better than nothing).
void restore();

/// Returns the current best-effort wall-clock epoch, or 0 if the clock was
/// never set.
time_t now();

/// True if the clock has been set at least once (NTP or restore).
bool isSynced();

/// Periodic callback (called from main loop) to compensate temperature-induced
/// RTC drift while the device is awake.  Runs at a 10-minute interval.
/// Computes the drift delta since the last baseline using the temperature
/// model and nudges the system clock by only that delta (the kernel clock
/// already advanced the raw amount).  Drift state is persisted to NVS only
/// in saveBeforeSleep() to minimise flash wear.
void updatePeriodic();

/// True if the last restore was from a backup (not NTP) — i.e. the clock
/// may have drifted.  Cleared on NTP sync.
bool isApproximate();

/// Returns the epoch of the last successful NTP sync (from NVS), or 0 if
/// no sync has ever been recorded.
time_t lastSyncTime();

/// Format the current time for display.  Returns "--:--" if the clock was
/// never synced, prefixes with "~" if approximate.
/// When use24h is false, formats as "2:05pm" / "12:30am".
/// Output is written to `buf` (must be at least 16 bytes).
void formatTime(char* buf, size_t bufSize, bool use24h);

/// Format the current time for log timestamps.  Returns "HH:MM:SS" if
/// synced, or an empty string if not.
void formatLogTime(char* buf, size_t bufSize);

/// Tear down WiFi cleanly.  When skipNtpSync is false (default) and the
/// clock is approximate, performs an opportunistic NTP sync before
/// disconnecting — essentially free since we already have a connection.
void wifiOff(bool skipNtpSync = false);

}  // namespace HalClock
