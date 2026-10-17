#pragma once

// Heap allocation tracing for on-device profiling (the `heaptrace` env only).
//
// ESP-IDF heap hooks record every internal-heap allocation and free, with the
// frame-pointer call stack of the allocation, into a static ring buffer. A
// drain task streams the records over USB serial as `@HT` lines, and heap
// walks (`snapshot()`) resynchronize the host's replay after dropped records.
// scripts/heap_trace.py decodes the stream; docs/heap-trace.md describes it.
//
// Every entry point compiles to nothing unless CROSSPOINT_HEAP_TRACE is set.

namespace HeapTrace {

#ifdef CROSSPOINT_HEAP_TRACE
// Starts recording and the drain task. Call once, after Serial.begin().
void begin();
// Requests a heap snapshot; the drain task performs it.
void requestSnapshot();
// Handles the argument of a `CMD:HEAPTRACE <arg>` serial command.
void handleCommand(const char* arg);
// Blocks trace output while held, so binary serial transfers stay contiguous.
void lockOutput();
void unlockOutput();
#else
inline void begin() {}
inline void requestSnapshot() {}
inline void handleCommand(const char*) {}
inline void lockOutput() {}
inline void unlockOutput() {}
#endif

struct OutputLock {
  OutputLock() { lockOutput(); }
  ~OutputLock() { unlockOutput(); }
  OutputLock(const OutputLock&) = delete;
  OutputLock& operator=(const OutputLock&) = delete;
};

}  // namespace HeapTrace
