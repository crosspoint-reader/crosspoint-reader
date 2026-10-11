#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "BlePageTurner.h"

// The page turner's rules, pure and constexpr so the tests can run them with a pretend clock
// and a pretend heap.
namespace bleturner {

// The stack needs both to start; after it started the reader still needs one 32 KiB block
// (InflateReader::RING_BYTES and the streaming miniz window).
inline constexpr size_t kMinimumFreeBytes = 65536;
inline constexpr size_t kMinimumLargestBlockBytes = 32768;
// A radio nobody connected to for this long stops: left on, it keeps the CPU at full speed
// and a device lying still eats its battery.
inline constexpr uint32_t kIdleOffMs = 5u * 60u * 1000u;

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
};

// The ONE answer to "may the radio be on now". Everything that starts, keeps or stops the
// radio asks here.
constexpr Why radioVerdict(const RadioInputs& in) {
  const bool stackFits = in.heap.freeBytes >= kMinimumFreeBytes && in.heap.largestBlock >= kMinimumLargestBlockBytes;
  switch (in.phase) {
    case Phase::Idle:
      if (!in.enabled) return Why::Off;
      if (in.storageBusy) return Why::StorageBusy;
      // Pairing is the user asking for the radio now: the book's conditions do not apply.
      if (in.where == Where::PairingScreen) return Why::Ok;
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
      if (stackFits) return Why::Ok;
      // Enough bytes in total, but no block the stack can take. A heap short in total is not
      // this: a restart cannot give back bytes the book really uses.
      return in.heap.freeBytes >= kMinimumFreeBytes ? Why::HeapInPieces : Why::HeapLow;
    case Phase::JustStarted:
      if (in.storageBusy) return Why::StorageBusy;
      // The stack may come up and take the reader's last large block. The check before the start
      // found the bytes, so a stack that leaves no whole block behind split the heap: in pieces.
      return in.heap.largestBlock >= kMinimumLargestBlockBytes ? Why::Ok : Why::HeapInPieces;
    case Phase::Running:
      if (!in.enabled) return Why::Off;
      if (in.storageBusy) return Why::StorageBusy;
      // A linked remote is never idle: it sits quiet between two presses for a long time.
      if (!in.linked && in.idleMs >= kIdleOffMs) return Why::IdleNoLink;
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
      // Read once: the next page turn gives the title back, whatever the radio does meanwhile.
      return in.acknowledged ? LinkNote::None : LinkNote::Failed;
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
