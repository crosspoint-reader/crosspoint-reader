#pragma once

#include <climits>
#include <cstdint>
#include <cstring>

/// The pure half of the boot/sleep diagnostics: the on-card record format, the ring layout
/// that holds it, and the rules for reading a record back.
///
/// Split out from BootDiagnostics so it can be exercised on the host. Every defect this
/// feature has shipped so far has been in exactly this logic and every one of them was
/// found by flashing a device: a stage read as a verdict before it was final, a reset
/// reason trusted to mean something the C3 cannot express, and a newest-first scan that
/// broke on its own record. None of that needs hardware to catch — it needs a test — and
/// none of it needs Arduino, the HAL, NVS or an SD card to express, so this header depends
/// on nothing but <cstdint> and <cstring>.
///
/// Everything here operates on a caller-supplied byte image; file I/O, RTC memory and NVS
/// stay in BootDiagnostics.cpp.
namespace BootDiag {

/// One line of the sleep/wake history. 16 bytes, POD, and read back with memcpy rather
/// than a pointer cast — the C3 faults on unaligned multi-byte loads.
struct Record {
  uint32_t seq;    // monotonic across boots and power cycles; establishes ordering
  uint8_t kind;    // Kind
  uint8_t code;    // Sleep: SleepStage | Boot: HalGPIO::WakeVerdict | Aborted: last verdict
  uint8_t reason;  // Sleep/Aborted: SleepTrigger | Boot: esp_reset_reason()
  uint8_t flags;   // Sleep: kFlag* | Boot: esp_sleep_get_wakeup_cause()
  uint16_t msA;    // Sleep: release-wait ms | Boot: gate decidedAtMs | Aborted: count
  uint16_t msB;    // Sleep: uptime seconds  | Boot: gate heldMs
  uint16_t msC;    // Sleep: unused          | Boot: millis() at SD mount
  uint16_t pad;    // keeps the struct 16 bytes and 4-byte aligned
};
static_assert(sizeof(Record) == 16, "Record is written to SD verbatim; keep it 16 bytes");

enum Kind : uint8_t {
  KindSleep = 0,
  KindBoot = 1,
  // Boots that woke, refused to come up, and went straight back to sleep. They are counted
  // in NVS as they happen (nothing else survives that path) and drained into the ring by
  // the next boot that actually completes — ONE record per distinct reason, carrying that
  // reason in `reason`/`code` and how many times it fired in `msA`. Per reason rather than
  // per run because "it refused 7 times" and "it refused 4 times for one reason and 3 for
  // another" are different faults.
  KindAborted = 2,
};

/// Record::flags bits, sleep records only.
enum SleepFlags : uint8_t {
  kFlagKeepClock = 1 << 0,       // battery latch stays HIGH, MCU powered through sleep
  kFlagFromReader = 1 << 1,      // slept with a book open
  kFlagReleaseTimeout = 1 << 2,  // the power-button release wait gave up
  kFlagStageFromRtc = 1 << 3,    // final stage was amended from the RTC breadcrumb
  // Set when the record is written, cleared by the next boot that finalises it. While set,
  // `code` is only the stage reached at write time — the last steps had not run yet — so it
  // must not be read as a verdict.
  kFlagInProgress = 1 << 4,
  // The breadcrumb did not survive, so the outcome was deduced from the fact that the rail
  // went away rather than measured.
  kFlagStageInferred = 1 << 5,
};

/// How far a deep-sleep attempt got. Ordered: a larger value is strictly more progress.
enum class SleepStage : uint8_t {
  None = 0,
  Requested,        // enterDeepSleep() entered and committed
  StatePersisted,   // app state + clock written
  ScreenPainted,    // sleep screen handed to the panel
  PanelAsleep,      // display.deepSleep() returned
  RailsConfigured,  // GPIO13 / rail teardown done, wake pin pulled up
  AwaitingRelease,  // blocking on the power-button release — where a stuck pin hangs
  ReleaseTimedOut,  // the release wait gave up and slept anyway
  WakeArmed,        // wake source armed; esp_deep_sleep_start() is the next statement
  Count,
};

/// Why the firmware decided to sleep.
enum class SleepTrigger : uint8_t {
  Unknown = 0,
  PowerHold,         // power button held past the sleep threshold
  Timeout,           // inactivity auto-sleep
  ButtonAction,      // a button mapped to the Sleep action
  WakeGateRejected,  // woke, gate refused the press, going straight back down
  UsbPowerBoot,      // USB power caused a cold boot; nothing else was initialised
  SerialCommand,     // a host sent CMD:SLEEP (or CMD:BTN:POWER:LONG) over USB serial
  Count,
};

/// How a sleep record's `code` should be read once finalised.
///
/// Deliberately does NOT lean on esp_reset_reason() to separate a clean power-down from a
/// reset-button rescue: on the ESP32-C3 it cannot. reset_reason.c has no ESP_RST_EXT case
/// at all — RESET_REASON_CHIP_POWER_ON covers both a real power-on and a CHIP_PU (EN pin)
/// reset — so the two boots are indistinguishable at that level by hardware.
enum class SleepOutcome : uint8_t {
  ReachedDeepSleep,   // measured: the breadcrumb survived and showed the wake source armed
  ReleaseTimedOut,    // measured: the release wait gave up, so the device was still running
  PoweredOffAsAsked,  // the rail was cut, which is what this sleep's policy asked for
  PoweredOffUnasked,  // the rail was cut although the policy said the MCU should stay up
  DidNotSleep,        // the recorded stage itself shows it never got there
  Unfinished,         // still flagged in-progress — the finalising boot never happened
};

inline SleepOutcome outcomeOf(const Record& record) {
  if (record.kind != KindSleep) {
    return SleepOutcome::DidNotSleep;
  }
  if ((record.flags & kFlagInProgress) != 0) {
    return SleepOutcome::Unfinished;
  }
  // Measured, and the only failure verdict that survives a rail cut: the release wait gave
  // up, so the device was still executing long after it should have been asleep.
  if ((record.flags & kFlagReleaseTimeout) != 0) {
    return SleepOutcome::ReleaseTimedOut;
  }
  if ((record.flags & kFlagStageInferred) != 0) {
    // All that is known is that the rail went away. Whether that is the right answer
    // depends on what this sleep asked for: a latch-cut sleep powering the board off is the
    // intended end, a latch-kept one is not.
    return (record.flags & kFlagKeepClock) != 0 ? SleepOutcome::PoweredOffUnasked : SleepOutcome::PoweredOffAsAsked;
  }
  return record.code >= static_cast<uint8_t>(SleepStage::WakeArmed) ? SleepOutcome::ReachedDeepSleep
                                                                    : SleepOutcome::DidNotSleep;
}

// ---------------------------------------------------------------------------
// Ring layout
// ---------------------------------------------------------------------------

/// How many events the ring keeps. 16 covers eight full sleep/wake cycles at 272 bytes on
/// the card.
inline constexpr uint8_t kCapacity = 16;

constexpr uint32_t kRingMagic = 0x57424431;  // 'WBD1'
/// Bumped to 2 when version-1 sleep records turned out to be indistinguishable from an
/// artefact (they were written at the PanelAsleep stage and never finalised on a device
/// whose sleep cuts the battery latch). Stale versions are discarded rather than shown.
constexpr uint16_t kRingVersion = 2;

struct RingHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t count;     // valid records, <= kCapacity
  uint16_t head;      // index of the OLDEST record
  uint16_t reserved;  // keeps nextSeq 4-byte aligned in the file image
  uint32_t nextSeq;
};
static_assert(sizeof(RingHeader) == 16, "RingHeader is written to SD verbatim");

inline constexpr size_t kImageBytes = sizeof(RingHeader) + sizeof(Record) * kCapacity;

inline RingHeader ringHeaderOf(const uint8_t* image) {
  RingHeader header{};
  memcpy(&header, image, sizeof(header));
  return header;
}

inline void ringSetHeader(uint8_t* image, const RingHeader& header) { memcpy(image, &header, sizeof(header)); }

inline void ringGet(const uint8_t* image, uint8_t slot, Record& out) {
  memcpy(&out, image + sizeof(RingHeader) + static_cast<size_t>(slot) * sizeof(Record), sizeof(Record));
}

inline void ringSet(uint8_t* image, uint8_t slot, const Record& in) {
  memcpy(image + sizeof(RingHeader) + static_cast<size_t>(slot) * sizeof(Record), &in, sizeof(Record));
}

/// Reset an image to an empty, valid ring.
inline void ringInit(uint8_t* image) {
  memset(image, 0, kImageBytes);
  const RingHeader header{kRingMagic, kRingVersion, 0, 0, 0, 1};
  ringSetHeader(image, header);
}

/// Whether an image read off the card is a ring this build can use.
inline bool ringValid(const uint8_t* image) {
  const RingHeader header = ringHeaderOf(image);
  return header.magic == kRingMagic && header.version == kRingVersion && header.count <= kCapacity &&
         header.head < kCapacity;
}

/// Slot holding the newest record, or kCapacity when the ring is empty.
inline uint8_t ringNewestSlot(const RingHeader& header) {
  if (header.count == 0) {
    return kCapacity;
  }
  return static_cast<uint8_t>((header.head + header.count - 1) % kCapacity);
}

/// Append `record`, stamping it with the next sequence number. Overwrites the oldest entry
/// once full.
inline void ringAppend(uint8_t* image, Record& record) {
  RingHeader header = ringHeaderOf(image);
  record.seq = header.nextSeq++;
  uint8_t slot;
  if (header.count < kCapacity) {
    slot = static_cast<uint8_t>((header.head + header.count) % kCapacity);
    header.count++;
  } else {
    slot = header.head;
    header.head = static_cast<uint16_t>((header.head + 1) % kCapacity);
  }
  ringSet(image, slot, record);
  ringSetHeader(image, header);
}

/// Copy up to `maxRecords` entries into `out`, newest first. Returns how many were filled.
inline uint8_t ringLoadNewestFirst(const uint8_t* image, Record* out, uint8_t maxRecords) {
  const RingHeader header = ringHeaderOf(image);
  const uint8_t wanted = header.count < maxRecords ? static_cast<uint8_t>(header.count) : maxRecords;
  for (uint8_t i = 0; i < wanted; i++) {
    const uint8_t slot = static_cast<uint8_t>((header.head + header.count - 1 - i + kCapacity) % kCapacity);
    ringGet(image, slot, out[i]);
  }
  return wanted;
}

/// Index of the first record belonging to the run that ended with THIS boot, and one past
/// its last, in a newest-first list.  Returns false when there is no such run.
///
/// Slot 0 is this boot's own record, because persistBoot() appends the abort summary before
/// it — so the walk has to step over one KindBoot and stop at the next, which is the
/// previous completed boot.  Getting that backwards is what made the abort counter read
/// zero for every device that had actually been looping.
inline bool currentRunRange(const Record* newestFirst, uint8_t count, uint8_t& first, uint8_t& last) {
  bool steppedOverOwnBoot = false;
  first = 0;
  last = 0;
  for (uint8_t i = 0; i < count; i++) {
    if (newestFirst[i].kind == KindBoot) {
      if (steppedOverOwnBoot) {
        last = i;
        return first < last;
      }
      steppedOverOwnBoot = true;
      first = static_cast<uint8_t>(i + 1);
      continue;
    }
  }
  last = count;
  return steppedOverOwnBoot && first < last;
}

/// Total aborted boots in the run that ended with this boot.  Sums every KindAborted record
/// in the run rather than taking the first: persistBoot() emits ONE record per distinct
/// abort reason, so a device that refused for two different reasons has two of them.
inline uint16_t abortsInCurrentRun(const Record* newestFirst, uint8_t count) {
  uint8_t first = 0;
  uint8_t last = 0;
  if (!currentRunRange(newestFirst, count, first, last)) {
    return 0;
  }
  uint32_t total = 0;
  for (uint8_t i = first; i < last; i++) {
    if (newestFirst[i].kind == KindAborted) {
      total += newestFirst[i].msA;
    }
  }
  return static_cast<uint16_t>(total > UINT16_MAX ? UINT16_MAX : total);
}

}  // namespace BootDiag
