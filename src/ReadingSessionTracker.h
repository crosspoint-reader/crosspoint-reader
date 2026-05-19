#pragma once
#include <cstdint>
#include <string>

// Tracks a single reading session and flushes its accumulated time/pages into
// the ReadingStatsStore when it ends.
//
// Design notes
//   - Idle-clamped (CrossPet/KOReader pattern). Each page-turn delta is capped
//     at MAX_IDLE_MS, so leaving a book open overnight doesn't poison totals.
//   - Two clock sources, mandatory dual-track:
//       * elapsedMs accumulator driven by millis() — always correct, survives
//         a never-synced HalClock.
//       * walltimeStartEpoch snapshot from HalClock::now() if synced — gives
//         "first read" / "last read" wallclock for the UI.
//     Whichever is available at flush time wins. If HalClock becomes synced
//     mid-session, the next flush starts producing real epochs without losing
//     the elapsed millis already accumulated.
//   - Lifecycle is owned by the reader activity:
//       begin(...) on reader enter
//       onPageTurn() on each forward/backward page turn
//       end()  on reader exit / sleep / power off
//     Multiple begin() calls without an end() are tolerated — they restart
//     the session, flushing the prior one first.
class ReadingSessionTracker {
 public:
  // Maximum gap (millis) between two activity events that still counts toward
  // session time. Beyond this, the gap is treated as idle and dropped.
  static constexpr uint32_t MAX_IDLE_MS = 90 * 1000;

  // Start a session for `docId`. If a previous session is still open it is
  // flushed first under its own docId.
  void begin(const std::string& docId, const std::string& title, const std::string& author);

  // Notify of any activity that should accumulate reading time. Typically
  // called from the reader's page-turn path. Safe to call before begin()
  // (no-op).
  void onPageTurn();

  // Update the snapshot of the book's progress (0-100). Cached and written
  // out when the session is flushed. Cheap; OK to call on every page turn.
  void updateProgress(uint8_t progress);

  // Flush the session into ReadingStatsStore and reset internal state.
  // Subsequent onPageTurn() calls are no-ops until begin() is called again.
  // Persists the stats file. If the session contributed no reading time it
  // is silently dropped (no entry created).
  void end();

  // True while a session is active (between begin() and end()).
  bool isActive() const { return active; }

  // Live read-only view of the in-flight session. Useful for UI ("you've
  // been reading for X minutes"). Returns 0 when no session is active.
  uint32_t getLiveSeconds() const;
  uint32_t getLivePages() const { return pagesTurnedThisSession; }
  const std::string& getDocId() const { return docId; }

 private:
  void flushIdleSinceLastActivity();

  bool active = false;
  std::string docId;
  std::string title;
  std::string author;

  // Wall clock anchor at begin() — 0 if HalClock wasn't synced then.
  // We don't refresh this if HalClock becomes synced mid-session; we just
  // pick it up on the next session.
  int64_t walltimeStartEpoch = 0;

  // millis() at the most recent activity (begin or page turn). Used to
  // compute the next idle-clamped delta.
  uint32_t lastActivityMs = 0;

  // Idle-clamped accumulator (milliseconds).
  uint64_t accumulatedMs = 0;

  uint32_t pagesTurnedThisSession = 0;
  uint8_t lastKnownProgress = 0;
};

// Global tracker singleton; readers route their lifecycle calls through here.
ReadingSessionTracker& globalReadingSessionTracker();
