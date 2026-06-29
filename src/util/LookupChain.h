#pragma once

#include <cstdint>
#include <vector>

// Back-navigation stack for chained dictionary lookups, stored compactly.
//
// Instead of owning a copy of each prior headword (a std::string per entry —
// multi-word phrase lookups make these heap-allocated and unbounded), each entry
// references the headword's position in the persisted lookup-history log by
// distance-from-newest, plus the page the user was reading. The owning activity
// resolves the actual word from the history log at that index when navigating
// back (which also re-derives the canonical title for stem/alt-form lookups).
//
// Two load-bearing invariants (documented because a naive impl gets them wrong):
//   1. Indices are distance-from-the-NEWEST history entry, so they stay valid
//      under front-eviction (oldest entries dropping out of the capped log).
//      An oldest=0 index would drift as the log evicts.
//   2. Stored indices are a NON-CONTIGUOUS subset of history positions. A
//      back-then-forward leaves the abandoned branch in the log as a gap
//      (back-nav neither logs nor removes). Example: chain A→B→C, back to B,
//      forward to D ⇒ log [A,B,C,D], stack [A@3, B@2], skipping the gap C@1.
//      The stack tracks exact positions, gaps and all.
//
// Pure: no I/O, no SD, no settings — host-unit-testable. The activity owns the
// history-log resolution and the cap value.
class LookupChain {
 public:
  struct Entry {
    uint8_t histIndex = 0;  // distance-from-newest of the headword in the history log
    uint16_t page = 0;      // page the user was on when they chained away
  };

  // historyCap = live history capacity (SETTINGS.getLookupHistoryCapValue()).
  // Entries whose referenced word would fall outside the capped log are dropped,
  // guaranteeing every remaining index always resolves.
  void reset(int historyCap) {
    entries_.clear();
    currentHistIndex_ = -1;
    cap_ = historyCap;
  }

  // Distance-from-newest of the currently displayed word (0 = newest).
  // -1 means the current word is not in the history log (cannot be referenced).
  void setCurrentHistIndex(int idx) { currentHistIndex_ = idx; }
  int currentHistIndex() const { return currentHistIndex_; }

  bool empty() const { return entries_.empty(); }
  int depth() const { return static_cast<int>(entries_.size()); }
  const Entry& at(int i) const { return entries_[i]; }  // entries_[0] is the oldest (bottom)

  // Forward navigation: leaving the current word (displayed on `page`) for a new
  // word. `appended` is true when the new word is being written to the history
  // log (true for every real forward lookup). Pushes a back-entry for the word
  // being left, then makes the new word current.
  void onForward(uint16_t page, bool appended) {
    if (currentHistIndex_ >= 0) {
      if (appended) {
        // The new word becomes the newest log entry, so every existing entry's
        // distance-from-newest grows by one.
        for (auto& e : entries_) {
          if (e.histIndex < 0xFF) e.histIndex = static_cast<uint8_t>(e.histIndex + 1);
        }
      }
      Entry e;
      e.histIndex = static_cast<uint8_t>(currentHistIndex_ + (appended ? 1 : 0));
      e.page = page;
      entries_.push_back(e);
      // Drop bottom entries whose referenced word has been evicted from the log.
      // Indices are monotonic (bottom = largest), so at most one crosses per push.
      while (!entries_.empty() && entries_.front().histIndex >= cap_) {
        entries_.erase(entries_.begin());
      }
    }
    currentHistIndex_ = appended ? 0 : -1;
  }

  // Back navigation: pop the most recent back-entry and make its word current.
  // Caller resolves the headword from the history log at the returned histIndex.
  Entry pop() {
    Entry e = entries_.back();
    entries_.pop_back();
    currentHistIndex_ = e.histIndex;
    return e;
  }

 private:
  std::vector<Entry> entries_;  // entries_.back() is the top of the back-stack
  int currentHistIndex_ = -1;
  int cap_ = 0;
};
