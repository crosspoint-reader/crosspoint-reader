#pragma once

#include <functional>
#include <string>
#include <vector>

#include "BlePageTurner.h"
#include "RadioPort.h"

// The radio (RadioPort.h, linked instead of SdkRadio.cpp) and the host (a bleturner::Host),
// both under the test's hand.
namespace fake {

struct Radio {
  bool running = false;
  bool stopping = false;
  bool connected = false;
  bool connecting = false;
  bool scanning = false;
  bool beginResult = true;
  bool endResult = true;
  unsigned beginCalls = 0;
  unsigned endCalls = 0;
  uint32_t lastEndTimeoutMs = 0;
  bool endDuringBegin = false;
  bool insideBegin = false;
  bool beginHadFullSpeed = false;
  bool endHadFullSpeed = false;
  // The stack takes heap: the host's heap reads this after a successful begin, and the heap
  // from before once end() has stopped it.
  bool changeHeapOnBegin = false;
  bleturner::Heap heapAfterBegin{};
  bleturner::Heap heapBeforeBegin{};
  std::function<void()> beginHook;
  std::string addr;
  std::string name;
  std::vector<bleturner::port::RawEdge> raw;
  std::vector<bleturner::port::KeyPress> keys;
  // "arm" and "poll", in order.
  std::vector<std::string> events;
  bool armResult = true;
  std::string armedAddr;
  unsigned armCalls = 0;
  unsigned polls = 0;
  uint32_t scanMs = 0;
  std::vector<std::string> connects;
  std::vector<std::string> forgotten;
  // The start task: queued until runStart() when queueStarts, else run at once.
  bool queueStarts = false;
  bool createSucceeds = true;
  unsigned creates = 0;
  bool startQueued = false;
  // Clock: sleepMs advances it, a sleep hook lets a test act "meanwhile".
  uint32_t now = 10000;
  std::function<void()> onSleep;
  bleturner::Memo memo{0};

  void runStart();
};
Radio& radio();

struct HostState {
  bleturner::Heap heap{65536, 32768};
  unsigned heapReads = 0;
  unsigned releaseCalls = 0;
  // The render lock is held by someone else: releaseCaches fails while true.
  bool renderBusy = false;
  std::vector<bleturner::Action> delivered;
  bool deliverResult = true;
  bool yieldResult = true;
  unsigned yields = 0;
  bool transfer = false;
  unsigned restarts = 0;
  bool fullSpeed = false;
  std::vector<std::string> logs;
  std::vector<std::string> maps;
};
HostState& host();
const bleturner::Host& hostFns();

// Everything back to a fresh boot, the module included; the RTC memo stays unless asked.
void reset(bool keepMemo = false);

// A book in front with its page shown.
bleturner::Scene reading(uint32_t visit = 1);

}  // namespace fake
