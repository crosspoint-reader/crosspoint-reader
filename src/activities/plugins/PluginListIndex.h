#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

// The optional browse.lists_url index: its lists follow the manifest's own
// ("fixed") lists.
namespace pluginlistindex {

enum class Outcome : uint8_t {
  Fetched,    // the index added lists
  FixedOnly,  // the fetch failed; the fixed lists are still shown
  Failed,     // the fetch failed and no list is left to show
};

inline Outcome outcomeOf(const bool addedLists, const size_t fixedCount) {
  if (addedLists) return Outcome::Fetched;
  return fixedCount > 0 ? Outcome::FixedOnly : Outcome::Failed;
}

// Keeps the first fixedCount lists, then adds each fetched entry for which
// read(source, list) returns true. Returns false if it adds none.
template <typename List, typename Range, typename Read>
bool replaceFetched(std::vector<List>& lists, const size_t fixedCount, const Range& fetched, const Read& read) {
  lists.resize(fixedCount);
  lists.reserve(fixedCount + fetched.size());
  for (const auto& source : fetched) {
    List list;
    if (read(source, list)) lists.push_back(std::move(list));
  }
  return lists.size() > fixedCount;
}

}  // namespace pluginlistindex
