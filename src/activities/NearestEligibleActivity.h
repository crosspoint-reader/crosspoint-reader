#pragma once

// Searches the visible activity first, then the stack from top to bottom. This is
// allocation-free; the single firmware instantiation operates directly on the
// ActivityManager's existing unique_ptr/vector storage.
template <typename ActivityPointer, typename ActivityStack, typename Predicate>
auto findNearestEligibleActivity(const ActivityPointer& current, const ActivityStack& stack, Predicate predicate)
    -> decltype(&*current) {
  if (current && predicate(*current)) return &*current;
  for (auto activity = stack.rbegin(); activity != stack.rend(); ++activity) {
    if (*activity && predicate(**activity)) return &**activity;
  }
  return nullptr;
}
