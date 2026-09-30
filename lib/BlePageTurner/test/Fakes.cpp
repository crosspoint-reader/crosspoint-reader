#include "Fakes.h"

#include <cstdio>
#include <stdexcept>

#include "Runtime.h"

namespace fake {
namespace {
Radio theRadio;
HostState theHost;

bleturner::Heap hostHeap() {
  ++theHost.heapReads;
  return theHost.heap;
}
bool hostRelease() {
  if (theHost.renderBusy) return false;
  ++theHost.releaseCalls;
  return true;
}
bool hostDeliver(const bleturner::Action a) {
  theHost.delivered.push_back(a);
  return theHost.deliverResult;
}
bool hostYield() {
  ++theHost.yields;
  return theHost.yieldResult;
}
bool hostTransfer() { return theHost.transfer; }
void hostRestart() { ++theHost.restarts; }
void hostHold(const bool hold) { theHost.fullSpeed = hold; }
void hostLog(bool error, const char* format, va_list args) {
  char line[256];
  vsnprintf(line, sizeof line, format, args);
  theHost.logs.push_back(std::string(error ? "ERR " : "INF ") + line);
}
void hostMap(const char* tag) { theHost.maps.push_back(tag); }
const bleturner::Host kHost{hostHeap,    hostRelease, hostDeliver, hostYield, hostTransfer,
                            hostRestart, hostHold,    hostLog,     hostMap};
}  // namespace

Radio& radio() { return theRadio; }
HostState& host() { return theHost; }
const bleturner::Host& hostFns() { return kHost; }

void Radio::runStart() {
  if (!startQueued) throw std::runtime_error("no start queued");
  startQueued = false;
  bleturner::detail::startTask();
}

void reset(const bool keepMemo) {
  const bleturner::Memo memo = theRadio.memo;
  theRadio = Radio{};
  if (keepMemo) theRadio.memo = memo;
  theHost = HostState{};
  bleturner::detail::resetForTests();
}

bleturner::Scene reading(const uint32_t visit) {
  bleturner::Scene s{};
  s.where = bleturner::Where::Reader;
  s.visit = visit;
  s.pageShown = true;
  return s;
}

}  // namespace fake

namespace bleturner::port {
using fake::theRadio;

bool compiledIn() { return true; }
bool begin() {
  ++theRadio.beginCalls;
  theRadio.insideBegin = true;
  theRadio.beginHadFullSpeed = fake::theHost.fullSpeed;
  if (theRadio.beginHook) theRadio.beginHook();
  if (theRadio.beginResult) {
    theRadio.running = true;
    if (theRadio.changeHeapOnBegin) {
      theRadio.heapBeforeBegin = fake::theHost.heap;
      fake::theHost.heap = theRadio.heapAfterBegin;
    }
  }
  theRadio.insideBegin = false;
  return theRadio.beginResult;
}
bool end(const uint32_t timeoutMs) {
  ++theRadio.endCalls;
  theRadio.lastEndTimeoutMs = timeoutMs;
  theRadio.endDuringBegin = theRadio.endDuringBegin || theRadio.insideBegin;
  theRadio.endHadFullSpeed = fake::theHost.fullSpeed;
  if (theRadio.changeHeapOnBegin && theRadio.running && theRadio.endResult)
    fake::theHost.heap = theRadio.heapBeforeBegin;
  theRadio.running = false;
  theRadio.connected = false;
  theRadio.stopping = !theRadio.endResult;
  return theRadio.endResult;
}
bool running() { return theRadio.running; }
bool stopping() { return theRadio.stopping; }
bool connected() { return theRadio.connected; }
bool connecting() { return theRadio.connecting; }
bool scanning() { return theRadio.scanning; }
void poll() {
  ++theRadio.polls;
  theRadio.events.emplace_back("poll");
}
bool popRaw(RawEdge& out) {
  if (theRadio.raw.empty()) return false;
  out = theRadio.raw.front();
  theRadio.raw.erase(theRadio.raw.begin());
  return true;
}
bool popKey(KeyPress& out) {
  if (theRadio.keys.empty()) return false;
  out = theRadio.keys.front();
  theRadio.keys.erase(theRadio.keys.begin());
  return true;
}
bool armReconnect(const char* addr) {
  ++theRadio.armCalls;
  theRadio.armedAddr = addr;
  theRadio.events.emplace_back("arm");
  return theRadio.armResult;
}
Peer linked() { return theRadio.connected ? Peer{theRadio.addr.c_str(), theRadio.name.c_str()} : Peer{"", ""}; }
void scan(const uint32_t durationMs) {
  theRadio.scanMs = durationMs;
  theRadio.scanning = durationMs != 0;
}
bool connect(const char* addr) {
  theRadio.connects.emplace_back(addr);
  return true;
}
void disconnect() { theRadio.connected = false; }
void forget(const char* addr) { theRadio.forgotten.emplace_back(addr); }
bool takeConnectFailure(char*, size_t) { return false; }
uint8_t bondCount() { return 0; }
Peer bond(uint8_t) { return {"", ""}; }
uint8_t foundCount() { return 0; }
Peer found(uint8_t) { return {"", ""}; }
bool spawnStart() {
  ++theRadio.creates;
  if (!theRadio.createSucceeds) return false;
  if (theRadio.startQueued) throw std::runtime_error("two starts queued");
  theRadio.startQueued = true;
  if (!theRadio.queueStarts) theRadio.runStart();
  return true;
}
uint32_t nowMs() { return theRadio.now; }
void sleepMs(const uint32_t ms) {
  theRadio.now += ms;
  // An hour of sleeps in one test is a wait that never ends: fail instead of hanging.
  if (theRadio.now > 10000u + 3600000u) throw std::runtime_error("waited an hour: a loop never ends");
  if (theRadio.onSleep) theRadio.onSleep();
}
Memo& restartMemo() { return theRadio.memo; }

}  // namespace bleturner::port
