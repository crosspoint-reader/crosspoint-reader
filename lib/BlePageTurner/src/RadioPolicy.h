#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "BlePageTurner.h"

// The page turner's rules, pure and constexpr so the tests can run them with a pretend clock
// and a pretend heap.
namespace bleturner {

// X3 numbers (the default profile below; the S3 has its own). The stack needs both to start; after it
// started the reader still needs one 32 KiB block (InflateReader::RING_BYTES and the streaming miniz
// window). Under kMinimumFreeBytes the heap is short in total; above it a refusal counts as a heap in
// pieces (shouldRestart).
inline constexpr size_t kMinimumFreeBytes = 65536;
inline constexpr size_t kMinimumLargestBlockBytes = 32768;
// The stack took 50,644 to 51,188 B in every X3 log (free before and after the start). A start with
// less than that plus the reader's block free can only roll back, and while it is up the render task
// runs out of heap beside it: X3, 07/10/2026, free 73,656 before the start, 22,676 after, then
// abort() in a page restore. It is refused before the stack comes up.
inline constexpr size_t kStackBytes = 49152;
inline constexpr size_t kStartFreeBytes = kStackBytes + kMinimumLargestBlockBytes;
// A radio nobody connected to for this long stops: left on, it keeps the CPU at full speed
// and a device lying still eats its battery.
inline constexpr uint32_t kIdleOffMs = 30u * 1000u;

// What a chip's stack costs and where the radio may start. X3 (C3, stack pools cut down): 49,152 B as
// logged in every X3 start, and the reader there needs a whole 32 KiB block (the inflate ring is a malloc
// in the same small heap). S3 with NimBLE's host in PSRAM (FREEINK_BLE_STACK_IN_PSRAM, set by
// scripts/s3_nimble_psram.h): the unit measured on 13/10/2026 went from free 124,752 / largest 81,908 at
// Home to 78,244 / 36,852, so the stack took 46,508 B of internal RAM and 45,056 B of the largest block.
struct Profile {
  size_t stackBytes;
  size_t startFreeBytes;    // stackBytes plus the reader's room
  size_t minimumLargest;    // the largest block needed before and after the stack
  size_t homeStartLargest;  // 0: the radio does not start at Home; else the largest block Home must have
};
inline constexpr size_t kS3StackBytes = 46508 + 4096;  // measured, plus a margin of one allocator page
inline constexpr size_t kS3StackLargestDrop = 45056;
// On the S3 malloc and new above 4,096 B go to PSRAM (prebuilt sdkconfig.h: CONFIG_SPIRAM_USE_MALLOC=1,
// CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096): the inflate ring, the layout buffers and the picture buffers never
// ask internal RAM for a block bigger than this. The heaviest page build on the host asks 16,560 B live in
// such requests and 4,096 B at most in one (HugeBookSection.S3InternalCensusOfTheHeaviestBuild), counting
// operator new only. Malloc and task stacks are not in that census, so the room kept contiguous after the stack
// is six of the largest request (24,576 B), wider than the four the census alone would justify.
inline constexpr size_t kS3InternalAllocMax = 4096;
inline constexpr size_t kS3MinimumLargest = 6 * kS3InternalAllocMax;
inline constexpr Profile kX3Profile{kStackBytes, kStartFreeBytes, kMinimumLargestBlockBytes, 0};
inline constexpr Profile kS3Profile{kS3StackBytes, kS3StackBytes + kS3MinimumLargest, kS3MinimumLargest,
                                    kS3MinimumLargest + kS3StackLargestDrop + 2048};
#ifdef FREEINK_BLE_STACK_IN_PSRAM
inline constexpr const Profile& activeProfile() { return kS3Profile; }
#else
inline constexpr const Profile& activeProfile() { return kX3Profile; }
#endif

constexpr bool radioRearmWanted(bool idleStopped, bool entered, bool requested, bool buildPainted) {
  return entered || requested || (buildPainted && !idleStopped);
}

// Where the question is asked.
enum class Phase : uint8_t {
  Idle,         // may this book visit ask for the radio (asked once per visit, running or not)
  BeforeStart,  // on the start task, caches released, right before the stack comes up
  JustStarted,  // the stack came up: keep it, or roll it back
  Running,      // the radio is up: keep it?
};

struct RadioInputs {
  Phase phase;
  bool enabled;
  Where where;
  bool pageShown;
  bool bookIndexing;
  bool storageBusy;
  bool heldForBuild;  // beforeChapterBuild() stopped it and afterPaint() has not come yet
  bool idleStopped;   // stopped for idleness and not asked again since
  bool linked;        // a remote is connected
  uint32_t idleMs;    // since a remote was last connected (or the radio came up)
  Heap heap;
  bool connectionBusy = false;
  bool hasBond = false;  // a remote has been chosen (Config::peerAddr): the only reason to start at Home
};

// The ONE answer to "may the radio be on now". Everything that starts, keeps or stops the
// radio asks here.
constexpr Why radioVerdict(const RadioInputs& in, const Profile& profile = activeProfile()) {
  const bool stackFits = in.heap.freeBytes >= profile.startFreeBytes && in.heap.largestBlock >= profile.minimumLargest;
  switch (in.phase) {
    case Phase::Idle:
      if (!in.enabled) return Why::Off;
      if (in.storageBusy) return Why::StorageBusy;
      // Pairing is the user asking for the radio now: the book's conditions do not apply.
      if (in.where == Where::PairingScreen) return Why::Ok;
      // S3: the stack goes in at Home, whose heap is whole, and the book inherits it.
      if (in.where == Where::Home) {
        if (profile.homeStartLargest == 0 || !in.hasBond) return Why::NotReading;
        if (!in.pageShown) return Why::PageNotShown;
        return in.idleStopped ? Why::IdleNoLink : Why::Ok;
      }
      if (in.where != Where::Reader) return Why::NotReading;
      if (!in.pageShown) return Why::PageNotShown;
      // A radio stopped for a starved build waits for the page, not for a key: restarting it
      // on a key release starved the build again and the start held the loop for 2.85 s (X3,
      // 23/09/2026).
      if (in.heldForBuild) return Why::BuildNeedsHeap;
      if (in.idleStopped) return Why::IdleNoLink;
      // A book still building its index needs the heap the radio would take; its keys work.
      if (in.bookIndexing) return Why::Indexing;
      return Why::Ok;
    case Phase::BeforeStart:
      if (in.storageBusy) return Why::StorageBusy;
      // A start at Home needs the block the reader will want after the stack; a refusal there is HeapLow, which
      // never asks for a restart into the book (there is none to restart into).
      if (in.where == Where::Home) {
        return stackFits && in.heap.largestBlock >= profile.homeStartLargest ? Why::Ok : Why::HeapLow;
      }
      if (stackFits) return Why::Ok;
      // Enough bytes in total, but no block the stack can take, or too few to leave the reader's
      // block beside the stack: the same verdict the rollback right after the start gave before
      // 07/10/2026, so the restart comes as it did. A heap short in total is not this: a restart
      // cannot give back bytes the book really uses.
      return in.heap.freeBytes >= kMinimumFreeBytes ? Why::HeapInPieces : Why::HeapLow;
    case Phase::JustStarted:
      if (in.storageBusy) return Why::StorageBusy;
      // The stack may come up and take the reader's last large block. The check before the start
      // found the bytes, so a stack that leaves no whole block behind split the heap: in pieces.
      return in.heap.largestBlock >= profile.minimumLargest ? Why::Ok : Why::HeapInPieces;
    case Phase::Running:
      if (!in.enabled) return Why::Off;
      if (in.storageBusy) return Why::StorageBusy;
      // A linked remote is never idle: it sits quiet between two presses for a long time.
      if (!in.linked && !in.connectionBusy && in.idleMs >= kIdleOffMs) return Why::IdleNoLink;
      return Why::Ok;
  }
  return Why::Off;
}

// --- The link note ---------------------------------------------------------------
// A radio up this long in one book visit with nothing linked has failed to find the remote.
inline constexpr uint32_t kLinkNoteFailMs = 20000;

struct NoteInputs {
  bool entered;  // this pass starts a book visit
  bool reading;  // a book is in front
  bool enabled;
  bool idleStopped;    // the radio is off for idleness: nothing will try to link
  bool linked;         // a remote is connected
  bool refused;        // this visit's radio start was refused (heap rules, rollback)
  uint32_t runningMs;  // how long the radio has been up without a break in this visit
  bool acknowledged;   // a page turn was applied
};

// The ONE answer to "what does the status bar say about the remote now".
constexpr LinkNote nextLinkNote(const LinkNote note, const NoteInputs& in) {
  if (!in.enabled || !in.reading) return LinkNote::None;
  // Every visit starts over. A remote still linked has nothing to wait for, and a radio off for
  // idleness does not try until a key of the device wakes it.
  if (in.entered) return in.linked || in.idleStopped ? LinkNote::None : LinkNote::Connecting;
  switch (note) {
    case LinkNote::Connecting:
      if (in.linked) return LinkNote::None;
      return in.refused || in.runningMs >= kLinkNoteFailMs ? LinkNote::Failed : LinkNote::Connecting;
    case LinkNote::Failed:
    case LinkNote::Connected:
    case LinkNote::LowMemory:
      // Read once: the next page turn gives the title back, whatever the radio does meanwhile.
      return in.acknowledged ? LinkNote::None : note;
    case LinkNote::None:
      return LinkNote::None;
  }
  return LinkNote::None;
}

// --- The heap restart ------------------------------------------------------------
// A heap in pieces keeps the radio off for good: leaving the book does not give the block
// back (X3, 27/09/2026: free=87308 largest=23540 in the book and on Home alike, 61428 after
// a restart). It shows before the start (enough bytes, no block the stack can take) or right
// after it (the stack came up and left no whole block for the reader: X3, 29/09/2026, the
// 5,000-chapter book, 7 of 7 starts rolled back at largest 26,612). A restart into the same
// book is the cure. It is allowed once until the radio comes up again, so a heap still in
// pieces after the restart cannot restart the device in a loop.
inline constexpr uint8_t kRefusalsBeforeRestart = 3;
inline constexpr uint32_t kRestartSpentMagic = 0x42485231u;

// The second decision, kept apart: restart to hand the radio a whole heap. `why` is the
// verdict that ended this start (before or right after the stack came up).
constexpr bool shouldRestart(const Why why, const uint8_t inPiecesInRow, const bool restartedSinceRadioUp) {
  return !restartedSinceRadioUp && why == Why::HeapInPieces && inPiecesInRow >= kRefusalsBeforeRestart;
}

// Kept in RTC memory, so it outlives ESP.restart. Power-on garbage is anything but the magic and
// reads as "no restart spent".
struct Memo {
  uint32_t magic;
};

// One per boot; only `memo` outlives the restart.
struct Tracker {
  Memo& memo;
  uint8_t fragmentedInRow = 0;
  // Asked by a refusal on the start task, read by the main loop, which restarts once a page is
  // shown.
  std::atomic<bool> wanted{false};

  // A start the heap ended: refused before the stack came up, or rolled back right after. A heap
  // in pieces counts toward the restart; a heap short in total breaks the streak (a restart
  // cannot give back bytes the book really uses) and drops a restart asked earlier. True when
  // this one asks for the restart.
  bool refused(const Why why) {
    if (why != Why::HeapInPieces) {
      inconclusive();
    } else if (fragmentedInRow < UINT8_MAX) {
      ++fragmentedInRow;
    }
    const bool restart = shouldRestart(why, fragmentedInRow, memo.magic == kRestartSpentMagic);
    if (restart) wanted.store(true, std::memory_order_release);
    return restart;
  }
  // The start ended for a reason other than the heap (the stack failed, the start was cancelled,
  // the card was taken): no sign of pieces, the streak starts again.
  void inconclusive() {
    fragmentedInRow = 0;
    wanted.store(false, std::memory_order_release);
  }
  void radioUp() {
    fragmentedInRow = 0;
    memo.magic = 0;
    wanted.store(false, std::memory_order_release);
  }
  void restarting() {
    memo.magic = kRestartSpentMagic;
    wanted.store(false, std::memory_order_release);
  }
};

}  // namespace bleturner
