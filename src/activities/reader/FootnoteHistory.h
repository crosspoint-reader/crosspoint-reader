#pragma once

#include <optional>

class FootnoteHistory {
 public:
  struct Position {
    int spineIndex;
    int pageNumber;
  };

  bool empty() const { return count == 0; }
  int depth() const { return count; }
  const Position* origin() const { return empty() ? nullptr : &positions[0]; }

  void push(const Position position) {
    if (count < MAX_DEPTH) positions[count++] = position;
    pendingSourceSpine = position.spineIndex;
  }

  std::optional<Position> pop() {
    pendingSourceSpine = -1;
    if (empty()) return std::nullopt;
    return positions[--count];
  }

  // Resolve only a link's landing page; ordinary page turns keep the return history.
  void resolveLinkTarget(const Position target) {
    if (pendingSourceSpine < 0) return;
    const bool crossesSpine = pendingSourceSpine != target.spineIndex;
    pendingSourceSpine = -1;
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
  static constexpr int MAX_DEPTH = 3;
  Position positions[MAX_DEPTH] = {};
  int count = 0;
  int pendingSourceSpine = -1;
};
