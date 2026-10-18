#include "BootDiagnostics.h"

#include <Arduino.h>
#include <CrossPointSettings.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Preferences.h>
#include <esp_attr.h>
#include <esp_sleep.h>
#include <esp_system.h>

#include <cstdio>
#include <cstring>
#include <iterator>
#include <numeric>

namespace BootDiag {
namespace {

// ---------------------------------------------------------------------------
// This boot's trace (.bss)
// ---------------------------------------------------------------------------

uint16_t s_phaseMs[static_cast<uint8_t>(BootPhase::Count)] = {};
// Which phases were reached — a stamp of 0 is a legal millis() value, so "reached"
// cannot be inferred from the value. 13 phases fit a uint16_t mask with room to spare.
uint16_t s_phaseReached = 0;
static_assert(static_cast<uint8_t>(BootPhase::Count) <= 16, "s_phaseReached mask is 16 bits wide");

HalGPIO::WakeCheck s_wakeCheck;

// Short labels so the whole trace fits one 256-byte ring-buffer entry (Logging.cpp).
constexpr const char* kPhaseNames[] = {"entry", "nvs",  "gate",  "hw",    "ser",   "recov", "sd",
                                       "cfg",   "disp", "paint", "store", "route", "rel"};
static_assert(sizeof(kPhaseNames) / sizeof(kPhaseNames[0]) == static_cast<size_t>(BootPhase::Count),
              "kPhaseNames must stay in sync with BootPhase");

// ---------------------------------------------------------------------------
// In-flight sleep breadcrumb (RTC_NOINIT)
// ---------------------------------------------------------------------------
//
// RTC_NOINIT rather than RTC_DATA: the point is to survive a panic, a watchdog reset
// and the user's own reset press — none of which is a clean restart — so the block must
// not be re-initialised by the startup code. It is uninitialised on a cold boot, which
// is what the magic guards against.
//
// 12 bytes of the 8 KB RTC-slow segment. The build already sits at ~68% of it, so this
// is deliberately three words and not a struct that will grow.
constexpr uint32_t kCrumbMagic = 0x57424331;  // 'WBC1'

RTC_NOINIT_ATTR uint32_t s_crumbMagic;
RTC_NOINIT_ATTR uint32_t s_crumbSleep;  // stage | trigger<<8 | flags<<16 | pending<<24
RTC_NOINIT_ATTR uint32_t s_crumbWait;   // release-wait milliseconds

// Set while a sleep has been recorded in the crumb but NOT yet written to SD — the two
// early-boot sleep paths run before Storage.begin(), so they can only leave a crumb and
// let the next boot persist it.
constexpr uint32_t kPendingShift = 24;

bool crumbValid() { return s_crumbMagic == kCrumbMagic; }

uint8_t crumbStage() { return static_cast<uint8_t>(s_crumbSleep & 0xFF); }
uint8_t crumbTrigger() { return static_cast<uint8_t>((s_crumbSleep >> 8) & 0xFF); }
uint8_t crumbFlags() { return static_cast<uint8_t>((s_crumbSleep >> 16) & 0xFF); }
bool crumbPending() { return ((s_crumbSleep >> kPendingShift) & 1u) != 0; }

void crumbStore(uint8_t stage, uint8_t trigger, uint8_t flags, bool pending) {
  s_crumbSleep = static_cast<uint32_t>(stage) | (static_cast<uint32_t>(trigger) << 8) |
                 (static_cast<uint32_t>(flags) << 16) | (static_cast<uint32_t>(pending ? 1u : 0u) << kPendingShift);
  s_crumbMagic = kCrumbMagic;
}

// ---------------------------------------------------------------------------
// Aborted-boot counter (NVS)
// ---------------------------------------------------------------------------
//
// Its own namespace rather than a few more keys in "Crosspoint": this is diagnostics, it
// is written from a path that runs before the settings layer is fully up, and a corrupt or
// cleared entry here must never be able to disturb a real setting.
constexpr char kAbortNamespace[] = "wbdiag";
// One blob rather than a key per bucket: a single read-modify-write per abort, and the
// layout can grow a bucket without stranding orphaned keys in the namespace.
// Key renamed with the layout: getBytes() refuses a size mismatch and returns 0 without
// touching the buffer, so a stale entry under the old name would silently read as "no
// aborts" rather than as an error. A fresh name makes the old one simply unreachable.
constexpr char kAbortBucketsKey[] = "abrtB2";

struct AbortTally {
  uint16_t buckets[kAbortBucketCount] = {};
  // Never cleared, unlike the buckets. Counts every abort this installation has recorded,
  // so "did the abort path run at all" can be answered independently of whether the drain
  // into the ring worked — otherwise those two failures look identical on the page, which
  // is exactly the ambiguity that left an X3 reporting nothing with no way to narrow it.
  uint32_t lifetime = 0;

  uint32_t total() const {
    // uint32_t seed, not 0: the accumulator takes its type from the seed, so a plain 0
    // would sum uint16_t buckets in an int and a saturated run could overflow it.
    return std::accumulate(std::begin(buckets), std::end(buckets), uint32_t{0});
  }
};

AbortTally readAbortTally() {
  AbortTally tally;
  Preferences nvs;
  if (!nvs.begin(kAbortNamespace, /*readOnly=*/true)) {
    return tally;  // namespace has never been written — no aborts to report
  }
  // Short read (an older layout, or a truncated entry) leaves the rest zeroed, which reads
  // as "no aborts in those buckets" — the safe direction.
  nvs.getBytes(kAbortBucketsKey, &tally, sizeof(tally));
  nvs.end();
  return tally;
}

/// Zero the per-run buckets, keeping the lifetime counter. Not nvs.clear(): the lifetime
/// total has to outlive every drain or it cannot answer "has this ever fired".
void clearAbortBuckets() {
  Preferences nvs;
  if (!nvs.begin(kAbortNamespace, /*readOnly=*/false)) {
    return;
  }
  AbortTally tally;
  nvs.getBytes(kAbortBucketsKey, &tally, sizeof(tally));
  memset(tally.buckets, 0, sizeof(tally.buckets));
  nvs.putBytes(kAbortBucketsKey, &tally, sizeof(tally));
  nvs.end();
}

// ---------------------------------------------------------------------------
// Persisted ring
// ---------------------------------------------------------------------------

constexpr char kRingPath[] = "/.crosspoint/bootdiag.bin";

// One 272-byte scratch image, reused by both writers. Static rather than stack: the sleep
// path runs on the loop task after the render task has been left holding its own buffers,
// and this codebase is sensitive to stack-into-heap spills (see the btnsample stack note in
// HalGPIO). Static rather than heap: it must work when the heap is too fragmented to serve
// anything, which is one of the states worth recording.
uint8_t s_image[kImageBytes];

// Thin bindings of the shared ring helpers to that one buffer. The layout and the ordering
// rules live in BootDiagRing.h so they can be exercised on the host; only the file I/O and
// the choice of buffer belong here.
RingHeader imageHeader() { return ringHeaderOf(s_image); }
void imageSetHeader(const RingHeader& header) { ringSetHeader(s_image, header); }
void imageGetRecord(uint8_t slot, Record& out) { ringGet(s_image, slot, out); }
void imageSetRecord(uint8_t slot, const Record& in) { ringSet(s_image, slot, in); }
void imageAppend(Record& record) { ringAppend(s_image, record); }

bool readImage() {
  memset(s_image, 0, sizeof(s_image));
  HalFile file;
  if (!Storage.openFileForRead("DIAG", kRingPath, file)) {
    return false;
  }
  const int read = file.read(s_image, sizeof(s_image));
  file.close();
  if (read != static_cast<int>(sizeof(s_image))) {
    return false;
  }
  return ringValid(s_image);
}

bool writeImage() {
  Storage.mkdir("/.crosspoint");
  HalFile file;
  if (!Storage.openFileForWrite("DIAG", kRingPath, file)) {
    return false;
  }
  const size_t written = file.write(s_image, sizeof(s_image));
  file.close();
  return written == sizeof(s_image);
}

/// Load the ring, apply `mutate`, write it back. Rebuilds the file from scratch when it
/// is missing or unreadable, so a corrupt ring self-heals instead of silently going
/// unrecorded for the rest of the device's life.
template <typename Fn>
bool updateRing(Fn mutate) {
  if (!Storage.ready()) {
    return false;
  }
  if (!readImage()) {
    ringInit(s_image);
  }
  mutate();
  return writeImage();
}

Record makeSleepRecord() {
  Record record{};
  record.kind = KindSleep;
  record.code = crumbStage();
  record.reason = crumbTrigger();
  // kFlagInProgress: this is written before the release wait and the wake arming, so the
  // stage in it is not yet a verdict. The next boot finalises it.
  record.flags = static_cast<uint8_t>(crumbFlags() | kFlagInProgress);
  record.msA = static_cast<uint16_t>(s_crumbWait > UINT16_MAX ? UINT16_MAX : s_crumbWait);
  const unsigned long uptimeS = millis() / 1000UL;
  record.msB = static_cast<uint16_t>(uptimeS > UINT16_MAX ? UINT16_MAX : uptimeS);
  return record;
}

/// True when this boot's reset reason proves the rail went away — nothing more.
///
/// It deliberately does NOT try to separate a clean power-down from a reset-button rescue.
/// On the ESP32-C3 that separation does not exist: reset_reason.c's get_reset_reason() has
/// no ESP_RST_EXT case at all, RESET_REASON_CHIP_POWER_ON covers both a real power-on and
/// a CHIP_PU (EN pin) reset, and both come back as ESP_RST_POWERON. An earlier version of
/// this file read POWERON as proof that an in-flight sleep had completed; on this chip that
/// reads a rescued hang as a healthy sleep, which is exactly backwards for the bug the page
/// exists to diagnose (issue #155). The record is finalised as "the rail was cut" and the
/// page qualifies that against the sleep's own policy instead.
bool resetImpliesRailWasCut(esp_reset_reason_t reason) {
  return reason == ESP_RST_DEEPSLEEP || reason == ESP_RST_POWERON;
}

/// Turn the newest sleep record from "as far as we had got when we wrote it" into a
/// verdict. Runs on the boot after the sleep, which is the first moment the outcome is
/// knowable at all.
void finalizeNewestSleep(bool hadCrumb, uint8_t crumbFinalStage, uint8_t crumbFlagBits, uint32_t waitMs,
                         esp_reset_reason_t resetReason) {
  const RingHeader header = ringHeaderOf(s_image);
  const uint8_t slot = ringNewestSlot(header);
  if (slot >= kCapacity) {
    return;
  }
  Record record{};
  imageGetRecord(slot, record);
  if (record.kind != KindSleep || (record.flags & kFlagInProgress) == 0) {
    return;  // already final, or the newest record is a boot (no sleep since)
  }

  if (hadCrumb) {
    // The MCU stayed powered through the sleep, so the breadcrumb is a measurement of
    // where it actually got to — including "it hung in the release wait".
    if (record.code < crumbFinalStage) {
      record.code = crumbFinalStage;
    }
    record.flags = static_cast<uint8_t>(record.flags | crumbFlagBits | kFlagStageFromRtc);
    record.msA = static_cast<uint16_t>(waitMs > UINT16_MAX ? UINT16_MAX : waitMs);
  } else if (resetImpliesRailWasCut(resetReason)) {
    // No breadcrumb and the rail went away. That is all this proves — see
    // resetImpliesRailWasCut(). Flag it inferred and leave the stage where it was:
    // outcomeOf() reads the flag plus the sleep's own latch policy rather than pretending
    // a stage was measured.
    record.flags = static_cast<uint8_t>(record.flags | kFlagStageInferred);
  }
  // Remaining case: no crumb and a reset reason that means code was still running. The
  // stage the record already carries IS the answer — leave it exactly as written.
  record.flags = static_cast<uint8_t>(record.flags & ~kFlagInProgress);
  imageSetRecord(slot, record);
}

}  // namespace

bool previousSessionEndedWithoutSleep() {
  // Newest first: [0] is this boot's own record, [1] is whatever preceded it. Two boots
  // back to back mean the session between them never reached the sleep path — a reset
  // while awake, a crash, or a rail cut. On the C3 that is the ONLY way to see it, since
  // the reset reason cannot separate a reset press from a power-on.
  Record recent[2] = {};
  if (loadRecords(recent, 2) < 2) {
    return false;  // nothing to compare against yet
  }
  return recent[0].kind == KindBoot && recent[1].kind == KindBoot;
}

// ---------------------------------------------------------------------------
// Boot phase trace
// ---------------------------------------------------------------------------

void markPhase(BootPhase phase) {
  const unsigned long ms = millis();
  s_phaseMs[static_cast<uint8_t>(phase)] = static_cast<uint16_t>(ms > UINT16_MAX ? UINT16_MAX : ms);
  s_phaseReached |= static_cast<uint16_t>(1u << static_cast<uint8_t>(phase));
}

bool phaseReached(BootPhase phase) { return (s_phaseReached & (1u << static_cast<uint8_t>(phase))) != 0; }

uint16_t phaseMs(BootPhase phase) { return s_phaseMs[static_cast<uint8_t>(phase)]; }

const char* phaseName(BootPhase phase) {
  const auto index = static_cast<uint8_t>(phase);
  return index < static_cast<uint8_t>(BootPhase::Count) ? kPhaseNames[index] : "?";
}

void setWakeCheck(const HalGPIO::WakeCheck& check) { s_wakeCheck = check; }

const HalGPIO::WakeCheck& wakeCheck() { return s_wakeCheck; }

void logSummary() {
  LOG_INF("BOOT", "Wake gate: %s (decided at %u ms, gate saw the press for %u ms, required %u ms)",
          HalGPIO::wakeVerdictName(s_wakeCheck.verdict), s_wakeCheck.decidedAtMs, s_wakeCheck.heldMs,
          CrossPointSettings::getPowerWakeHoldDuration());

  // One "name+costMs" token per reached phase, then the two absolute numbers worth
  // quoting. Deltas rather than absolutes because the cost of a phase is the actionable
  // figure and absolutes are just their running sum — and because the whole line has to
  // fit one 256-byte ring-buffer entry including logPrintf's timestamp/level prefix
  // (Logging.cpp, MAX_ENTRY_LEN). Unreached phases are skipped (e.g. `ser` on battery,
  // `recov` on a non-button boot), so a short trace is itself a signal about which path
  // the boot took.
  char line[160];
  size_t used = 0;
  uint16_t previous = 0;
  for (uint8_t i = 0; i < static_cast<uint8_t>(BootPhase::Count); i++) {
    if ((s_phaseReached & (1u << i)) == 0) continue;
    const int written = snprintf(line + used, sizeof(line) - used, "%s%s+%u", used ? " " : "", kPhaseNames[i],
                                 static_cast<unsigned>(s_phaseMs[i] - previous));
    if (written < 0 || static_cast<size_t>(written) >= sizeof(line) - used) break;
    used += static_cast<size_t>(written);
    previous = s_phaseMs[i];
  }
  // millis() excludes the ROM/2nd-stage bootloader (~200-300 ms), so time-to-logo as the
  // user experiences it is that much longer than `logo` reports.
  // Worst case (five-digit stamps throughout) still fits MAX_ENTRY_LEN: 29 prefix + 15
  // here + 160 line + 38 tail = 242 of 256. Keep that budget in mind when editing either.
  LOG_INF("BOOT", "phase cost ms: %s | logo=%u setup=%u (+bootloader)", line,
          s_phaseMs[static_cast<uint8_t>(BootPhase::FirstPaint)], previous);
}

// ---------------------------------------------------------------------------
// Sleep breadcrumb
// ---------------------------------------------------------------------------

const char* stageName(SleepStage stage) {
  switch (stage) {
    case SleepStage::None:
      return "none";
    case SleepStage::Requested:
      return "requested";
    case SleepStage::StatePersisted:
      return "state-saved";
    case SleepStage::ScreenPainted:
      return "screen-painted";
    case SleepStage::PanelAsleep:
      return "panel-asleep";
    case SleepStage::RailsConfigured:
      return "rails-set";
    case SleepStage::AwaitingRelease:
      return "await-release";
    case SleepStage::ReleaseTimedOut:
      return "release-timeout";
    case SleepStage::WakeArmed:
      return "wake-armed";
    case SleepStage::Count:
      break;
  }
  return "?";
}

const char* triggerName(SleepTrigger trigger) {
  switch (trigger) {
    case SleepTrigger::Unknown:
      return "unknown";
    case SleepTrigger::PowerHold:
      return "power-hold";
    case SleepTrigger::Timeout:
      return "timeout";
    case SleepTrigger::ButtonAction:
      return "button";
    case SleepTrigger::WakeGateRejected:
      return "gate-rejected";
    case SleepTrigger::UsbPowerBoot:
      return "usb-boot";
    case SleepTrigger::SerialCommand:
      return "serial";
    case SleepTrigger::Count:
      break;
  }
  return "?";
}

const char* outcomeName(SleepOutcome outcome) {
  switch (outcome) {
    case SleepOutcome::ReachedDeepSleep:
      return "slept";
    case SleepOutcome::ReleaseTimedOut:
      return "release-timeout";
    case SleepOutcome::PoweredOffAsAsked:
      return "powered-off";
    case SleepOutcome::PoweredOffUnasked:
      return "powered-off-unasked";
    case SleepOutcome::DidNotSleep:
      return "did-not-sleep";
    case SleepOutcome::Unfinished:
      return "unfinished";
  }
  return "?";
}

const char* resetReasonName(uint8_t reason) {
  switch (static_cast<esp_reset_reason_t>(reason)) {
    case ESP_RST_UNKNOWN:
      return "unknown";
    case ESP_RST_POWERON:
      return "power-on";
    case ESP_RST_EXT:
      return "ext-reset";
    case ESP_RST_SW:
      return "sw-restart";
    case ESP_RST_PANIC:
      return "panic";
    case ESP_RST_INT_WDT:
      return "int-wdt";
    case ESP_RST_TASK_WDT:
      return "task-wdt";
    case ESP_RST_WDT:
      return "other-wdt";
    case ESP_RST_DEEPSLEEP:
      return "deep-sleep";
    case ESP_RST_BROWNOUT:
      return "brownout";
    case ESP_RST_SDIO:
      return "sdio";
    // The C3 reports a reset through its USB-Serial-JTAG port (flashing, a serial monitor
    // opening the port) as USB, not as a power-on.
    case ESP_RST_USB:
      return "usb";
    case ESP_RST_JTAG:
      return "jtag";
    case ESP_RST_EFUSE:
      return "efuse";
    case ESP_RST_PWR_GLITCH:
      return "power-glitch";
    case ESP_RST_CPU_LOCKUP:
      return "cpu-lockup";
    default:
      return "other";
  }
}

const char* wakeCauseName(uint8_t cause) {
  switch (static_cast<esp_sleep_wakeup_cause_t>(cause)) {
    case ESP_SLEEP_WAKEUP_UNDEFINED:
      return "none";
    // The S3 boards wake on the power button through EXT1; the C3 has no EXT wake and uses GPIO.
    case ESP_SLEEP_WAKEUP_EXT0:
      return "ext0";
    case ESP_SLEEP_WAKEUP_EXT1:
      return "ext1";
    case ESP_SLEEP_WAKEUP_TIMER:
      return "timer";
    case ESP_SLEEP_WAKEUP_GPIO:
      return "gpio";
    case ESP_SLEEP_WAKEUP_UART:
      return "uart";
    case ESP_SLEEP_WAKEUP_ULP:
      return "ulp";
    default:
      return "other";
  }
}

void beginSleep(SleepTrigger trigger, bool keepClockAlive, bool fromReader) {
  uint8_t flags = 0;
  if (keepClockAlive) flags |= kFlagKeepClock;
  if (fromReader) flags |= kFlagFromReader;
  s_crumbWait = 0;
  crumbStore(static_cast<uint8_t>(SleepStage::Requested), static_cast<uint8_t>(trigger), flags, /*pending=*/true);
}

void markSleepStage(SleepStage stage) {
  if (!crumbValid()) {
    // A stage without a beginSleep() means an unexpected call order rather than a real
    // sleep; record it as an unknown trigger rather than dropping it, so the anomaly is
    // visible on the page instead of silently absent.
    crumbStore(static_cast<uint8_t>(stage), static_cast<uint8_t>(SleepTrigger::Unknown), 0, /*pending=*/true);
    return;
  }
  if (static_cast<uint8_t>(stage) <= crumbStage()) {
    return;  // never walk the breadcrumb backwards
  }
  crumbStore(static_cast<uint8_t>(stage), crumbTrigger(), crumbFlags(), crumbPending());
}

void noteReleaseWait(unsigned long waitedMs, bool timedOut) {
  s_crumbWait = static_cast<uint32_t>(waitedMs);
  if (!crumbValid()) {
    return;
  }
  if (timedOut) {
    crumbStore(static_cast<uint8_t>(SleepStage::ReleaseTimedOut), crumbTrigger(),
               static_cast<uint8_t>(crumbFlags() | kFlagReleaseTimeout), crumbPending());
  }
}

// ---------------------------------------------------------------------------
// Persisted history
// ---------------------------------------------------------------------------

void persistSleep() {
  if (!crumbValid()) {
    return;
  }
  const bool ok = updateRing([] {
    Record record = makeSleepRecord();
    imageAppend(record);
  });
  if (ok) {
    // Written — the next boot amends this record in place instead of appending a second
    // one for the same sleep.
    crumbStore(crumbStage(), crumbTrigger(), crumbFlags(), /*pending=*/false);
  }
}

void noteAbortedBoot(SleepTrigger trigger, HalGPIO::WakeVerdict verdict) {
  const uint8_t bucket = abortBucketOf(trigger, verdict);
  Preferences nvs;
  if (!nvs.begin(kAbortNamespace, /*readOnly=*/false)) {
    LOG_ERR("BOOT", "Aborted boot could not be counted: NVS namespace unavailable");
    return;
  }
  AbortTally tally;
  nvs.getBytes(kAbortBucketsKey, &tally, sizeof(tally));
  if (tally.buckets[bucket] < kAbortCountCap) {
    tally.buckets[bucket]++;
    tally.lifetime++;
    const size_t written = nvs.putBytes(kAbortBucketsKey, &tally, sizeof(tally));
    if (written != sizeof(tally)) {
      LOG_ERR("BOOT", "Aborted boot could not be counted: NVS write returned %u", static_cast<unsigned>(written));
    }
  }
  nvs.end();
  LOG_INF("BOOT", "Aborted boot: %s / gate %s (x%u this run, %lu lifetime) — going straight back to sleep",
          triggerName(trigger), HalGPIO::wakeVerdictName(verdict), tally.buckets[bucket],
          static_cast<unsigned long>(tally.lifetime));
}

void persistReleaseTimeout(bool storageLive) {
  if (!storageLive) {
    return;  // this board's teardown path has already cut the SD rail
  }
  updateRing([] {
    const RingHeader header = ringHeaderOf(s_image);
    const uint8_t slot = ringNewestSlot(header);
    if (slot >= kCapacity) {
      return;
    }
    Record record{};
    imageGetRecord(slot, record);
    if (record.kind != KindSleep) {
      return;
    }
    record.code = static_cast<uint8_t>(SleepStage::ReleaseTimedOut);
    record.flags = static_cast<uint8_t>(record.flags | kFlagReleaseTimeout);
    record.msA = static_cast<uint16_t>(s_crumbWait > UINT16_MAX ? UINT16_MAX : s_crumbWait);
    imageSetRecord(slot, record);
  });
}

void persistBoot() {
  const bool hadCrumb = crumbValid();
  const uint8_t finalStage = hadCrumb ? crumbStage() : 0;
  const bool pending = hadCrumb && crumbPending();
  const uint8_t crumbFlagBits = hadCrumb ? crumbFlags() : 0;
  const uint8_t crumbTriggerBits = hadCrumb ? crumbTrigger() : 0;
  const uint32_t waitMs = hadCrumb ? s_crumbWait : 0;

  const esp_reset_reason_t resetReason = esp_reset_reason();
  const esp_sleep_wakeup_cause_t wakeupCause = esp_sleep_get_wakeup_cause();
  const AbortTally abortTally = readAbortTally();

  const bool ringWritten = updateRing([&] {
    if (hadCrumb && pending) {
      // The sleep never reached persistSleep() — one of the two early-boot paths, which
      // sleep before Storage.begin() and so could only leave a breadcrumb. Write it now,
      // already final: the breadcrumb it came from is the complete story.
      Record record{};
      record.kind = KindSleep;
      record.code = finalStage;
      record.reason = crumbTriggerBits;
      record.flags = static_cast<uint8_t>(crumbFlagBits | kFlagStageFromRtc);
      record.msA = static_cast<uint16_t>(waitMs > UINT16_MAX ? UINT16_MAX : waitMs);
      imageAppend(record);
    } else {
      // Unconditional, crumb or no crumb: a sleep that cut the rail leaves no breadcrumb
      // to amend from, and skipping the finalise there was what left every healthy X4
      // sleep reading as "never reached deep sleep".
      finalizeNewestSleep(hadCrumb, finalStage, crumbFlagBits, waitMs, resetReason);
    }

    // Aborted boots since the last completed one, placed before this boot's record so the
    // history reads chronologically. Nothing else could have recorded them: those boots
    // return before Storage.begin() and lose the breadcrumb with the rail.
    //
    // One record per non-zero bucket, not one per run: a device refusing four times because
    // the press was already over and three times because it was released early is a
    // different fault from one that only ever did the first, and a single total hides that.
    // At most kAbortBucketCount records, and in practice one or two.
    for (uint8_t bucket = 0; bucket < kAbortBucketCount; bucket++) {
      if (abortTally.buckets[bucket] == 0) {
        continue;
      }
      Record aborted{};
      aborted.kind = KindAborted;
      aborted.code = static_cast<uint8_t>(abortBucketVerdict(bucket));
      aborted.reason = static_cast<uint8_t>(abortBucketTrigger(bucket));
      aborted.msA = abortTally.buckets[bucket];
      imageAppend(aborted);
    }

    Record boot{};
    boot.kind = KindBoot;
    boot.code = static_cast<uint8_t>(s_wakeCheck.verdict);
    boot.reason = static_cast<uint8_t>(resetReason);
    boot.flags = static_cast<uint8_t>(wakeupCause);
    boot.msA = s_wakeCheck.decidedAtMs;
    boot.msB = s_wakeCheck.heldMs;
    boot.msC = phaseMs(BootPhase::SdMount);
    imageAppend(boot);
  });

  if (abortTally.total() > 0 && ringWritten) {
    // Cleared only once the ring actually took the summary — a failed card write would
    // otherwise discard the count and the aborts would vanish entirely. Guarded on
    // count > 0 as well, so the healthy path stays free of NVS writes.
    clearAbortBuckets();
  }

  // Consumed either way: a crumb that could not be written is still spent, and leaving it
  // set would attribute this boot's sleep state to the next boot as well.
  s_crumbMagic = 0;
  s_crumbSleep = 0;
  s_crumbWait = 0;
}

AbortCounts abortCounts() {
  const AbortTally tally = readAbortTally();
  AbortCounts counts;
  counts.lifetime = tally.lifetime;
  const uint32_t undrained = tally.total();
  counts.undrained = static_cast<uint16_t>(undrained > UINT16_MAX ? UINT16_MAX : undrained);
  return counts;
}

uint8_t loadRecords(Record* out, uint8_t maxRecords) {
  if (out == nullptr || maxRecords == 0 || !Storage.ready() || !readImage()) {
    return 0;
  }
  return ringLoadNewestFirst(s_image, out, maxRecords);
}

}  // namespace BootDiag
