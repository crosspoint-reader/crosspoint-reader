#include "../../src/activities/reader/ReaderProgressPolicy.h"

#include <cassert>
#include <cstdint>
#include <limits>

int main() {
  assert(ReaderProgressPolicy::persistablePageCount(false, 12) == 0);
  assert(ReaderProgressPolicy::persistablePageCount(true, 0) == 0);
  assert(ReaderProgressPolicy::persistablePageCount(true, 12) == 12);
  assert(ReaderProgressPolicy::persistablePageCount(true, 70000) == std::numeric_limits<uint16_t>::max());

  const auto unknownCount =
      ReaderProgressPolicy::decideResumeRemap(2, 2, 5, 0, false, 0);
  assert(!unknownCount.deferUntilComplete);
  assert(!unknownCount.applyPage);
  assert(unknownCount.pageNumber == 5);

  const auto otherSpine =
      ReaderProgressPolicy::decideResumeRemap(3, 2, 5, 20, false, 0);
  assert(!otherSpine.deferUntilComplete);
  assert(!otherSpine.applyPage);
  assert(otherSpine.pageNumber == 5);

  const auto incomplete =
      ReaderProgressPolicy::decideResumeRemap(2, 2, 5, 20, false, 0);
  assert(incomplete.deferUntilComplete);
  assert(!incomplete.applyPage);
  assert(incomplete.pageNumber == 5);

  const auto samePageCount =
      ReaderProgressPolicy::decideResumeRemap(2, 2, 5, 20, true, 20);
  assert(!samePageCount.deferUntilComplete);
  assert(!samePageCount.applyPage);
  assert(samePageCount.pageNumber == 5);

  const auto remapped =
      ReaderProgressPolicy::decideResumeRemap(2, 2, 5, 20, true, 40);
  assert(!remapped.deferUntilComplete);
  assert(remapped.applyPage);
  assert(remapped.pageNumber == 10);

  const auto clamped =
      ReaderProgressPolicy::decideResumeRemap(2, 2, 20, 20, true, 40);
  assert(clamped.applyPage);
  assert(clamped.pageNumber == 39);
}
