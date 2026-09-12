#pragma once

#include <cstddef>

namespace storage_test {

struct State {
  int lockDepth = 0;
  int maxLockDepth = 0;
  unsigned lockTakes = 0;
  unsigned lockGives = 0;
  unsigned openCalls = 0;
  unsigned nextCalls = 0;
  unsigned activeHandles = 0;
  unsigned closeCalls = 0;
  unsigned nextEntries = 0;
  unsigned allocations = 0;
  unsigned nothrowAllocations = 0;
  unsigned throwingAllocations = 0;
  unsigned errors = 0;
  bool unlockedOperation = false;
  bool openSucceeds = true;
  bool closeSucceeds = true;
  bool countAllocations = false;
  bool failAllocation = false;
};

inline State state;

inline void requireLock() {
  if (state.lockDepth == 0) state.unlockedOperation = true;
}

}  // namespace storage_test
