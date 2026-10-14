#pragma once

#include <algorithm>
#include <optional>

class FootnoteHistory {
 public:
  static constexpr int MAX_DEPTH = 3;

  struct Position {
    int spineIndex;
    int pageNumber;
  };

  bool empty() const { return count == 0; }
  int depth() const { return count; }
  const Position* data() const { return positions; }

  bool restore(const Position* saved, const int savedCount) {
    if (savedCount < 0 || savedCount > MAX_DEPTH || (savedCount > 0 && !saved)) return false;
    for (int i = 0; i < savedCount; ++i) positions[i] = saved[i];
    count = savedCount;
    pendingSourceSpine = -1;
    pendingEvictedPosition.reset();
    return true;
  }

  void push(const Position position) {
    pendingEvictedPosition.reset();
    if (count == MAX_DEPTH) {
      pendingEvictedPosition = positions[0];
      std::copy(positions + 1, positions + MAX_DEPTH, positions);
      --count;
    }
    positions[count++] = position;
    pendingSourceSpine = position.spineIndex;
  }

  std::optional<Position> pop() {
    pendingSourceSpine = -1;
    pendingEvictedPosition.reset();
    if (empty()) return std::nullopt;
    return positions[--count];
  }

  // Resolve only a link's landing page; ordinary page turns keep the return history.
  void resolveLinkTarget(const Position target) {
    if (pendingSourceSpine < 0) return;
    const bool crossesSpine = pendingSourceSpine != target.spineIndex;
    pendingSourceSpine = -1;
    // The backlink's push may have displaced the origin at the depth limit.
    const bool returnsToEvicted = pendingEvictedPosition && pendingEvictedPosition->spineIndex == target.spineIndex &&
                                  (crossesSpine || pendingEvictedPosition->pageNumber == target.pageNumber);
    pendingEvictedPosition.reset();
    if (returnsToEvicted) {
      count = 0;
      return;
    }
    for (int i = 0; i < count; ++i) {
      // Cross-spine backlinks can land on a nearby page; same-spine notes need a page match.
      if (positions[i].spineIndex == target.spineIndex &&
          (crossesSpine || positions[i].pageNumber == target.pageNumber)) {
        count = i;
        return;
      }
    }
  }

 private:
  Position positions[MAX_DEPTH] = {};
  int count = 0;
  int pendingSourceSpine = -1;
  std::optional<Position> pendingEvictedPosition;
};
