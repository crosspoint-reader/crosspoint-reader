#pragma once

#include <HalGPIO.h>

#include <cstdint>

#include "BootDiagRing.h"

/// Boot and sleep forensics.
///
/// Two things this firmware could not previously answer from the device itself:
///
///   1. Where did the last sleep stop?  A device that hangs before
///      esp_deep_sleep_start() and a device that slept fine but did not wake look
///      identical from the outside — the panel holds the sleep image either way and
///      no button does anything.  Issue #155 sat open for weeks on exactly that
///      ambiguity.
///   2. How did this boot start, and where did its time go?  The phase trace below
///      used to live as file-static state in main.cpp, reachable only over a serial
///      monitor (`CMD:BOOTLOG`) — which most reporters do not have, and which a
///      battery-powered device does not even open.
///
/// The answers are kept in three places, cheapest first:
///   * `.bss`      — this boot's phase stamps and wake verdict (26 bytes).
///   * RTC_NOINIT  — the in-flight sleep breadcrumb (12 bytes).  Survives a soft
///                   reset and a deep-sleep wake; lost whenever the rail is cut,
///                   which on an X4 sleeping with the default useClock=0 is EVERY
///                   normal sleep.  So its absence is evidence too, and the record
///                   is finalised from the reset reason in that case — see
///                   outcomeOf() and resetImpliesMcuHadStopped().
///   * SD          — a fixed-size ring of the last 16 sleep/boot events, so history
///                   survives power loss and can be read back on screen.
///
/// Surfaced by BootDiagnosticsActivity (Settings > System > Boot Diagnostics).
namespace BootDiag {

// ---------------------------------------------------------------------------
// Boot phase trace
// ---------------------------------------------------------------------------

/// millis() stamp per boot phase.  The wake gesture is only a ~300 ms gate
/// (getPowerWakeHoldDuration), but the splash lands seconds later because the SD
/// mount, the config loads, the panel bring-up and the first (non-differential)
/// waveform all sit between the two.  Without per-phase stamps that gap is invisible
/// in the log — every phase before Serial.begin() has no output at all — and "the
/// power button feels unresponsive" reports cannot be attributed to a phase.
///
/// The stamps stay live for the whole session precisely so they can be re-emitted: a
/// wake from deep sleep re-enumerates USB, so a host monitor reconnecting mid-boot
/// misses the first lines, and the RTC ring buffer (16 entries) has usually rolled
/// past them by the time anyone looks.
///
/// Cost is 2 bytes per phase of .bss plus one log line; no heap, no allocation.
/// Stamps are clamped to 16 bits (a boot that reaches 65 s is pathological and reads
/// as 65535).
enum class BootPhase : uint8_t {
  SetupEntry,      // first statement of setup()
  NvsSettings,     // startup settings read from NVS (precedes the wake gate)
  WakeGate,        // power-button wake gesture decided
  HwInit,          // system / SPI bus / GPIO / power / tilt brought up
  SerialUp,        // USB-CDC opened (only when USB is connected; earlier phases log nothing)
  RecoverySettle,  // UP+POWER recovery-combo sample window
  SdMount,         // Storage.begin()
  ConfigLoad,      // settings, app state, i18n, KOReader, OPDS, weather, theme
  DisplayFonts,    // panel init + framebuffer alloc + font registration + SD font scan
  FirstPaint,      // splash / quick-resume frame handed to the panel (logo now visible)
  StoreLoad,       // clock, recent books, bookmarks, reading stats
  ActivityRoute,   // target activity entered (home / reader / recovery)
  PowerRelease,    // wake press released (stable), input sampler about to start
  Count,
};

void markPhase(BootPhase phase);
bool phaseReached(BootPhase phase);
uint16_t phaseMs(BootPhase phase);
/// Short label ("entry", "gate", "sd", ...).  Points at a string literal.
const char* phaseName(BootPhase phase);

/// Record what the boot-time wake gate decided.  Kept for the whole session so it can
/// be re-emitted alongside the phase trace and shown on the diagnostics page.
void setWakeCheck(const HalGPIO::WakeCheck& check);
const HalGPIO::WakeCheck& wakeCheck();

/// The whole boot story in two log lines: what the power button was judged to be, and
/// where the time went.  Re-emitted on every serial (re)connect.
void logSummary();

// ---------------------------------------------------------------------------
// Sleep breadcrumb
// ---------------------------------------------------------------------------

const char* stageName(SleepStage stage);
const char* triggerName(SleepTrigger trigger);

/// Short tokens for an esp_reset_reason() and an esp_sleep_get_wakeup_cause() value, as a boot
/// record stores them. Deliberately untranslated: they are compared across bug reports, so a
/// German device and an English one must produce the same word. Point at string literals.
const char* resetReasonName(uint8_t reason);
const char* wakeCauseName(uint8_t cause);
/// One token per SleepOutcome ("slept", "release-timeout", ...). The device page folds two of
/// them into one compact marker; the web report keeps them apart.
const char* outcomeName(SleepOutcome outcome);

/// Open a sleep attempt.  Resets the breadcrumb and records the policy this sleep runs
/// under, so a later boot can say whether the battery latch was meant to be cut.
void beginSleep(SleepTrigger trigger, bool keepClockAlive, bool fromReader);

/// Advance the breadcrumb.  Never moves backwards, so an out-of-order call cannot
/// understate the progress actually made.
void markSleepStage(SleepStage stage);

/// Record the outcome of the power-button release wait: how long it blocked, and
/// whether it gave up rather than seeing a clean release.
void noteReleaseWait(unsigned long waitedMs, bool timedOut);

// ---------------------------------------------------------------------------
// Persisted history
// ---------------------------------------------------------------------------

/// Call once after Storage.begin().  Amends the previous sleep record from the RTC
/// breadcrumb (when it survived), appends this boot's record, and clears the
/// breadcrumb.  Cheap: one 272-byte read and one 272-byte write.
void persistBoot();

/// Record that this boot decided not to come up: the wake gate refused the press, or USB
/// power caused a cold boot.  Both paths return from setup() before Storage.begin(), so
/// they write no boot record, and on an X4 sleeping with the latch cut the RTC breadcrumb
/// does not survive the rail going away either.  Without this an abort loop is completely
/// invisible — the device reads as dead while it is in fact waking and refusing several
/// times a minute, which is one of the shapes issue #155 could take.
///
/// Counted in NVS because that is the only store alive at this point in setup() that also
/// survives the rail: the card is not mounted yet and RTC memory is about to be lost.  The
/// count saturates at kAbortCountCap so a runaway loop cannot write without bound, and the
/// healthy path costs one read and no writes.
void noteAbortedBoot(SleepTrigger trigger, HalGPIO::WakeVerdict verdict);

/// Ceiling on each NVS abort bucket.  Past this the run is self-evidently a loop and the
/// exact number stops being interesting, so writing stops.
inline constexpr uint16_t kAbortCountCap = 250;

/// Aborts are bucketed by reason, not just totalled: "it refused 7 times" and "it refused 4
/// times because the press was already over and 3 because it was released early" are
/// different faults, and only the second tells you which way to look.
///
/// Bucket 0 is the USB-power cold boot, whose wake verdict carries no information (the gate
/// runs before the branch is chosen and always reports whatever the pin happened to be
/// doing).  Buckets 1..N are wake-gate rejections keyed by the verdict that caused them, so
/// released-early and never-pressed stay separate.
inline constexpr uint8_t kWakeVerdictCount = 6;  // HalGPIO::WakeVerdict, NotPressed..NoSecondPress
inline constexpr uint8_t kAbortBucketUsbPower = 0;
inline constexpr uint8_t kAbortBucketCount = 1 + kWakeVerdictCount;

/// Bucket index for a (trigger, verdict) pair.
inline uint8_t abortBucketOf(SleepTrigger trigger, HalGPIO::WakeVerdict verdict) {
  if (trigger != SleepTrigger::WakeGateRejected) {
    return kAbortBucketUsbPower;
  }
  const uint8_t v = static_cast<uint8_t>(verdict);
  return v < kWakeVerdictCount ? static_cast<uint8_t>(1 + v) : kAbortBucketUsbPower;
}

/// Inverse of abortBucketOf(), for rendering a drained record.
inline SleepTrigger abortBucketTrigger(uint8_t bucket) {
  return bucket == kAbortBucketUsbPower ? SleepTrigger::UsbPowerBoot : SleepTrigger::WakeGateRejected;
}
inline HalGPIO::WakeVerdict abortBucketVerdict(uint8_t bucket) {
  return bucket == kAbortBucketUsbPower ? HalGPIO::WakeVerdict::NotPressed
                                        : static_cast<HalGPIO::WakeVerdict>(bucket - 1);
}

/// Call on the sleep path once everything that can fail has succeeded and only the
/// release wait and the wake arming remain.  Appends the sleep record so it survives a
/// power cut, a hang, or a battery pull.
void persistSleep();

/// Persist "the power-button release wait gave up" into the sleep record already on the
/// card.  Called only when the wait actually timed out, which is the one moment on this
/// path where writing is both safe and worth it: the device is provably still executing
/// (so no rail is collapsing under the write) and provably in the pathological state.  A
/// clean release must NOT write here — on an X4 sleeping with the latch cut, the rail
/// drops the instant the button opens, and a write into a collapsing rail is how SD cards
/// lose their FAT.  `storageLive` comes from the HAL: the rail-teardown branch of
/// startDeepSleep() has already cut the card by this point.
void persistReleaseTimeout(bool storageLive);

/// Abort counters read straight out of NVS, independent of the ring.
///
/// `lifetime` is never cleared, so a zero there means the abort path has never run on this
/// installation — which is the only way to tell "it never aborted" apart from "it aborted
/// and the drain into the ring lost it".  `undrained` is what the current run has counted
/// but no completed boot has filed yet; normally zero by the time anything can read it.
struct AbortCounts {
  uint32_t lifetime = 0;
  uint16_t undrained = 0;
};
AbortCounts abortCounts();

/// Read the ring back, newest first.  `out` must have room for kCapacity records.
/// Returns how many were filled.  No heap: the caller owns the storage.
uint8_t loadRecords(Record* out, uint8_t maxRecords);

/// True when this boot's record is not preceded by a sleep — the previous session ended
/// without reaching the sleep path at all (a reset while awake, a crash, a rail cut).  On
/// the C3 this is the only way to see that, since the reset reason cannot separate a reset
/// press from a power-on.
bool previousSessionEndedWithoutSleep();

}  // namespace BootDiag
