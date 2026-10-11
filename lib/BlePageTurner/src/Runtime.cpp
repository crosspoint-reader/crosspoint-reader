#include "Runtime.h"

#include <atomic>
#include <cstdarg>
#include <cstring>

#include "BlePageTurner.h"
#include "RadioPolicy.h"
#include "RadioPort.h"

namespace bleturner {
namespace {

// Until begin(): a build without the radio still answers every call.
Heap noHeap() { return {0, 0}; }
bool noRelease() { return true; }
bool noDeliver(Action) { return false; }
bool noYield() { return true; }
bool noTransfer() { return false; }
void noRestart() {}
void noHold(bool) {}
void noLog(bool, const char*, va_list) {}
constexpr Host kNoHost{noHeap, noRelease, noDeliver, noYield, noTransfer, noRestart, noHold, noLog, nullptr};
Config noConfig;

const Host* host = &kNoHost;
Config* config = &noConfig;

// A start refused for memory is retried a few times once the reader has settled: the first
// page build holds the heap the radio needs and gives it back a few seconds later when the
// builder parks.
constexpr uint32_t kRetryMs = 5000;
constexpr uint8_t kRetryLimit = 6;
// How long a starved chapter build waits for a start to settle, and for the radio to stop.
constexpr uint32_t kBuildReleaseTimeoutMs = 3000;

// One start at a time, across tasks. Written by the starter, read while the start task
// finishes; the task clears it last.
std::atomic<bool> attemptInFlight{false};
std::atomic<bool> attemptCancelled{false};
// A chapter build waits for the heap: a start still waiting for the render lock gives up
// its turn instead of waiting out the build that holds it.
std::atomic<bool> buildWaiting{false};
std::atomic<bool> readerStartDeferred{false};
std::atomic<bool> radioIdleStopped{false};
std::atomic<bool> rearmRequested{false};
std::atomic<bool> heldForBuild{false};
// Written by the start (one at a time), read by the main loop once no start is in flight.
Tracker heapTracker{port::restartMemo()};

// Book visit state, main loop only.
struct Visit {
  uint32_t id = 0;
  // Each visit asks for the radio once, so a refusal does not churn allocations.
  bool attempted = false;
  bool reconnectArmed = false;
  uint32_t retryAtMs = 0;
  uint8_t retries = 0;
};
Visit visit;
uint32_t lastCleanupMs = 0;
uint32_t idleSinceMs = 0;
// The linked remote's button table (learned, or the built-in default for its kind) and the
// tap/hold decision still waiting.
Router router;
Scene lastScene{};
// The remote the settings screen is pairing: it becomes the chosen one once it links. Main loop only.
struct Pairing {
  char addr[sizeof(Config::peerAddr)] = "";
  char name[sizeof(Config::peerName)] = "";
};
Pairing pairing;
// The book's link note (nextLinkNote): written on the main loop, read by the render task.
std::atomic<LinkNote> linkNoteNow{LinkNote::None};
// Since when the radio has been up without a break in this book visit. Main loop only.
bool noteRadioUp = false;
uint32_t noteRadioUpSinceMs = 0;

void say(const bool error, const char* format, ...) {
  va_list args;
  va_start(args, format);
  host->log(error, format, args);
  va_end(args);
}

RadioInputs inputsFor(const Phase phase, const Scene& s) {
  RadioInputs in{};
  in.phase = phase;
  in.enabled = config->enabled != 0;
  in.where = s.where;
  in.pageShown = s.pageShown;
  in.bookIndexing = s.bookIndexing;
  in.storageBusy = s.storageBusy;
  in.heldForBuild = heldForBuild.load(std::memory_order_relaxed);
  in.idleStopped = radioIdleStopped.load(std::memory_order_relaxed);
  in.linked = true;
  return in;
}

RadioInputs heapInputs(const Phase phase, const Heap heap, const bool storageBusy) {
  RadioInputs in{};
  in.phase = phase;
  in.storageBusy = storageBusy;
  in.heap = heap;
  return in;
}

void logSkipped(const char* reason, const Heap& heap) {
  say(true, "HID begin skipped (%s): free=%zu largest=%zu required_free=%zu required_largest=%zu\n", reason,
      heap.freeBytes, heap.largestBlock, kMinimumFreeBytes, kMinimumLargestBlockBytes);
  static uint8_t mapsLeft = 2;
  if (mapsLeft > 0 && host->heapMap != nullptr) {
    mapsLeft--;
    host->heapMap("ble-skipped");
  }
}

bool beginOwned() {
  if (attemptCancelled.load(std::memory_order_acquire) || port::stopping()) return false;
  if (port::running()) return true;

  const Heap before = host->heap();
  if (radioVerdict(heapInputs(Phase::BeforeStart, before, host->fileTransferActive())) == Why::StorageBusy) {
    logSkipped("filetransfer-active", before);
    return false;
  }

  // Font caches are rebuildable and can occupy a large fragmented block; the host frees them
  // under its render lock. Only the release holds that lock, never the stack start, so a
  // paint in flight is not starved by radio init.
  while (!host->releaseCaches()) {
    if (attemptCancelled.load(std::memory_order_acquire) || buildWaiting.load(std::memory_order_acquire)) return false;
    port::sleepMs(10);
  }

  const Heap heap = host->heap();
  const Why checked = radioVerdict(heapInputs(Phase::BeforeStart, heap, false));
  if (checked != Why::Ok) {
    logSkipped("insufficient-internal-heap", heap);
    heapTracker.refused(checked);
    return false;
  }

#ifdef BLE_PAGE_TURNER_PROBE
  // What the stack comes up in: the reader released its layout parser before this (yieldForRadio).
  say(false, "HID begin heap free=%zu largest=%zu\n", heap.freeBytes, heap.largestBlock);
#endif
  // One attempt. Callers decide when a later one is allowed; there is no retry loop here.
  if (attemptCancelled.load(std::memory_order_acquire) || host->fileTransferActive() || !port::begin()) {
    heapTracker.inconclusive();
    return false;
  }
  if (attemptCancelled.load(std::memory_order_acquire)) {
    heapTracker.inconclusive();
    port::end(0);
    return false;
  }

  const Heap after = host->heap();
  const Why started = radioVerdict(heapInputs(Phase::JustStarted, after, host->fileTransferActive()));
  if (started == Why::HeapInPieces) {
    say(true, "HID begin rolled back (post-init-headroom): free=%zu largest=%zu required_largest=%zu\n",
        after.freeBytes, after.largestBlock, kMinimumLargestBlockBytes);
    heapTracker.refused(started);
  } else if (started != Why::Ok) {
    heapTracker.inconclusive();
  }
  if (started != Why::Ok) {
    // The caller is already outside the render lock, so a bounded end keeps the loop moving.
    port::end(0);
    return false;
  }
#ifdef BLE_PAGE_TURNER_PROBE
  say(false, "HID begin kept free=%zu largest=%zu\n", after.freeBytes, after.largestBlock);
#endif
  heapTracker.radioUp();
  return true;
}

void finishAttempt(const bool started, const bool reportReaderFailure) {
  if (reportReaderFailure) {
    readerStartDeferred.store(!started && !attemptCancelled.load(std::memory_order_acquire), std::memory_order_relaxed);
  }
  if (!port::running() && !port::stopping()) host->holdFullSpeed(false);
  attemptInFlight.store(false, std::memory_order_release);
}

bool startSync() {
  radioIdleStopped.store(false, std::memory_order_relaxed);
  readerStartDeferred.store(false, std::memory_order_relaxed);
  bool expected = false;
  if (!attemptInFlight.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return false;
  attemptCancelled.store(false, std::memory_order_release);
  host->holdFullSpeed(true);
  const bool started = beginOwned();
  finishAttempt(started, false);
  return started;
}

bool busy() { return attemptInFlight.load(std::memory_order_acquire) || port::running() || port::stopping(); }

// A remote press: page and chapter moves count only when the book took them; the two
// shortcuts count as soon as they are asked for.
bool act(const Action a) {
  if (a == Action::None) return false;
  const bool taken = host->deliver(a);
  return taken || a == Action::ReaderMenu || a == Action::SaveQuote;
}

// A linked remote whose presses the book takes: the chosen one, or any when none is chosen.
// When the chosen remote could not be armed, the stack may link another bonded remote.
bool chosenLinked() {
  if (!port::connected()) return false;
  return config->peerAddr[0] == '\0' || strncmp(port::linked().addr, config->peerAddr, sizeof(config->peerAddr)) == 0;
}

// The book in front with the radio up: arm the chosen remote, drain both queues.
bool serveReader() {
  bool acted = false;
  readerStartDeferred.store(false, std::memory_order_relaxed);
  if (!visit.reconnectArmed && !port::stopping()) {
    visit.reconnectArmed = true;
    if (config->peerAddr[0] != '\0' && !port::armReconnect(config->peerAddr)) {
      say(false, "Selected reader peer was not armed\n");
    }
  }
  port::poll();
  // A remote with a table goes by RAW edges: each edge checks at most 8 slots of the linked
  // remote's table, and a button the table does not name falls back to the key mapping.
  // Without a table the raw ring is only drained and the key path below decides.
  port::RawEdge raw;
  port::KeyPress key;
  const bool linked = chosenLinked();
  router.follow(linked);
  if (port::connected() && !linked) {
    unsigned dropped = 0;
    while (port::popRaw(raw)) ++dropped;
    while (port::popKey(key)) ++dropped;
    if (dropped > 0) say(false, "%u presses from %s dropped: not the chosen remote\n", dropped, port::linked().addr);
    return false;
  }
  if (router.linked && !router.chosen) {
    // Edges queued while the book was not in front (the remote stays linked on Home) do not
    // belong to this page: an old press must not skip a chapter when the book opens.
    while (port::popRaw(raw)) {
    }
    const Peer peer = port::linked();
    router.table = tableFor(config->remotes, config->remoteCount, peer.addr, peer.name);
    router.chosen = true;
  }
  const bool viaTable = routes(router.table);
  while (port::popRaw(raw)) {
    if (!viaTable) continue;
    const Action a = onRawEdge(*router.table, raw.code(), raw.pressed, raw.atMs,
                               pageActionFor(*config, raw.keycode, raw.mods), router.wait);
    acted = act(a) || acted;  // act first, log after: the log line is not on the page's clock
    say(false, "raw %u:%u=%02X %s -> %s\n", raw.reportId, raw.byteIndex, raw.value, raw.pressed ? "down" : "up",
        actionName(a));
  }
  if (viaTable) {
    const Action held = pollHold(router.wait, port::nowMs());
    acted = act(held) || acted;
    if (held != Action::None) say(false, "raw hold -> %s\n", actionName(held));
  }
  while (port::popKey(key)) {
    // With a table the raw edge already decided this frame: its key event is only logged.
    const Action a = viaTable ? Action::None : pageActionFor(*config, key.keycode, key.mods);
    // One line for EVERY key taken: the diagnostic path for someone holding a real remote.
    say(false, "key 0x%02X mods 0x%02X %s -> %s\n", key.keycode, key.mods, key.pressed ? "down" : "up",
        a == Action::PrevPage   ? "previous"
        : a == Action::NextPage ? "next"
                                : "none");
    if (a != Action::PrevPage && a != Action::NextPage) continue;
    // Only the press acts. Free3 reports a fixed release 60-100 ms after every press however
    // long the button is held, so the release frame carries nothing to act on.
    if (!key.pressed) continue;
    if (host->deliver(a)) acted = true;
  }
  return acted;
}

void stepLinkNote(const bool entered, const bool acknowledged) {
  NoteInputs in{};
  in.entered = entered;
  in.reading = lastScene.where == Where::Reader;
  in.enabled = config->enabled != 0;
  in.idleStopped = radioIdleStopped.load(std::memory_order_relaxed);
  in.linked = chosenLinked();
  in.refused = visit.attempted && readerStartDeferred.load(std::memory_order_relaxed) &&
               !attemptInFlight.load(std::memory_order_acquire);
  in.runningMs = noteRadioUp ? port::nowMs() - noteRadioUpSinceMs : 0;
  in.acknowledged = acknowledged;
  linkNoteNow.store(nextLinkNote(linkNoteNow.load(std::memory_order_relaxed), in), std::memory_order_relaxed);
}

}  // namespace

namespace detail {

void startTask() {
  const bool started = beginOwned();
  finishAttempt(started, true);
}

bool startAsync() {
  radioIdleStopped.store(false, std::memory_order_relaxed);
  readerStartDeferred.store(false, std::memory_order_relaxed);
  bool expected = false;
  if (!attemptInFlight.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return false;
  if (port::stopping() || port::running()) {
    const bool running = port::running();
    attemptInFlight.store(false, std::memory_order_release);
    return running;
  }
  attemptCancelled.store(false, std::memory_order_release);
  host->holdFullSpeed(true);
  if (!port::spawnStart()) {
    finishAttempt(false, true);
    return false;
  }
  return true;
}

bool suspend(const uint32_t endTimeoutMs) {
  // Never stop the stack while a start is still in flight: the start task owns its
  // allocations until finishAttempt publishes completion.
  attemptCancelled.store(true, std::memory_order_release);
  if (attemptInFlight.load(std::memory_order_acquire)) return false;
  if (port::running() || port::stopping()) {
    host->holdFullSpeed(true);
    if (!port::end(endTimeoutMs)) return false;
  }
  host->holdFullSpeed(false);
  return true;
}

bool stopForIdle() {
  radioIdleStopped.store(true, std::memory_order_relaxed);
  readerStartDeferred.store(false, std::memory_order_relaxed);
  return suspend(0);
}

bool heapRestartWanted() { return heapTracker.wanted.load(std::memory_order_acquire); }

#ifdef BLETURNER_TESTING
void resetForTests() {
  host = &kNoHost;
  config = &noConfig;
  noConfig = Config{};
  attemptInFlight.store(false);
  attemptCancelled.store(false);
  buildWaiting.store(false);
  readerStartDeferred.store(false);
  radioIdleStopped.store(false);
  rearmRequested.store(false);
  heldForBuild.store(false);
  heapTracker.fragmentedInRow = 0;
  heapTracker.wanted.store(false);
  visit = Visit{};
  lastCleanupMs = 0;
  idleSinceMs = 0;
  router = Router();
  lastScene = Scene{};
  pairing = Pairing{};
  linkNoteNow.store(LinkNote::None);
  noteRadioUp = false;
  noteRadioUpSinceMs = 0;
}
#endif

}  // namespace detail

void begin(const Host& h, Config& c) {
  host = &h;
  config = &c;
}

bool tick(const Scene& s) {
  bool acted = false;
  // A book visit starts: the book comes in front, or another visit of it does.
  const bool entered = s.where == Where::Reader && (lastScene.where != Where::Reader || s.visit != lastScene.visit);
  lastScene = s;
  // Finish a stop in the background without blocking input.
  if (port::stopping() && port::nowMs() - lastCleanupMs >= 250) {
    lastCleanupMs = port::nowMs();
    detail::suspend(0);
  }
  // The card is taken (USB drive, file transfer): before anything else, radio off.
  if (s.storageBusy) {
    idleSinceMs = 0;
    detail::suspend(0);
  }

  const bool reader = s.where == Where::Reader;
  if (!reader || !config->enabled || s.visit != visit.id) {
    // A build hold belongs to one book visit; the next visit may start the radio again.
    if (s.visit != visit.id) heldForBuild.store(false, std::memory_order_relaxed);
    visit.attempted = false;
    visit.reconnectArmed = false;
    router = Router();  // the settings screen may have changed the tables meanwhile
    visit.retryAtMs = 0;
    visit.retries = 0;
  }
  visit.id = s.visit;
  // A page key or touch in the book grants one fresh start after the idle stop. A radio the
  // book stopped for a starved build waits for the book's own request (afterPaint).
  if (reader && config->enabled && radioIdleStopped.load(std::memory_order_relaxed) &&
      ((s.localKey && !heldForBuild.load(std::memory_order_relaxed)) ||
       rearmRequested.exchange(false, std::memory_order_relaxed))) {
    radioIdleStopped.store(false, std::memory_order_relaxed);
    visit.attempted = false;
    visit.reconnectArmed = false;
    say(false, "Reader input rearmed idle radio\n");
  }

  RadioInputs in = inputsFor(Phase::Running, s);
  const Why must = radioVerdict(in);
  if (must == Why::Off || must == Why::StorageBusy) {
    idleSinceMs = 0;
    detail::suspend(0);
  } else {
    if (reader && visit.attempted && readerStartDeferred.load(std::memory_order_relaxed) && !port::running() &&
        !attemptInFlight.load(std::memory_order_acquire) && !radioIdleStopped.load(std::memory_order_relaxed) &&
        visit.retries < kRetryLimit) {
      if (visit.retryAtMs == 0) {
        visit.retryAtMs = port::nowMs() + kRetryMs;
      } else if (port::nowMs() >= visit.retryAtMs) {
        visit.retryAtMs = 0;
        ++visit.retries;
        visit.attempted = false;
        say(false, "Retrying reader BLE start after memory refusal (%u)\n", static_cast<unsigned>(visit.retries));
      }
    } else {
      visit.retryAtMs = 0;
      if (port::running()) visit.retries = 0;
    }
    // The saved opt-in starts only once the book has painted a page: Home needs its own
    // cover and font memory first. The book frees what it can right before (yieldForRadio).
    in.phase = Phase::Idle;
    if (!visit.attempted && !port::stopping() && radioVerdict(in) == Why::Ok &&
        (port::running() || host->yieldForRadio())) {
      visit.attempted = true;
      if (!port::running()) {
        const bool started = detail::startAsync();
        if (!started) readerStartDeferred.store(true, std::memory_order_relaxed);
        if (started) {
          say(false, "Reader BLE start requested\n");
        } else {
          say(true, "Reader BLE start deferred: insufficient memory or unavailable radio\n");
        }
      }
    }
    // Nothing connected for long: radio down. Otherwise it keeps the CPU at full speed and a
    // device lying still eats its battery.
    if (!attemptInFlight.load(std::memory_order_acquire) && port::running()) {
      in.linked = chosenLinked();
      if (in.linked || idleSinceMs == 0) idleSinceMs = port::nowMs();
      in.phase = Phase::Running;
      in.idleMs = port::nowMs() - idleSinceMs;
      if (radioVerdict(in) == Why::IdleNoLink) {
        say(false, "Radio idle for %u ms with nothing connected; stopping\n", kIdleOffMs);
        detail::stopForIdle();
        idleSinceMs = 0;
      }
    } else {
      idleSinceMs = 0;
    }
    if (reader && !attemptInFlight.load(std::memory_order_acquire) && port::running()) acted = serveReader();
  }

  const bool up = reader && port::running() && !attemptInFlight.load(std::memory_order_acquire);
  if (!up) {
    noteRadioUp = false;
  } else if (!noteRadioUp || entered) {
    noteRadioUp = true;
    noteRadioUpSinceMs = port::nowMs();
  }
  stepLinkNote(entered, false);

  // A heap in pieces keeps the radio off until a restart. Restart into the book only from a
  // shown page with no radio start in flight, no sleep, no card or Wi-Fi session.
  if (detail::heapRestartWanted() && config->enabled && visit.attempted && reader && s.pageShown && !s.sleeping &&
      !s.storageBusy && !busy() && !s.wifiOn) {
    const Heap heap = host->heap();
    say(false, "Heap fragmented for radio: free=%u largest=%u; silent restart to reader\n",
        static_cast<unsigned>(heap.freeBytes), static_cast<unsigned>(heap.largestBlock));
    heapTracker.restarting();
    host->restartIntoBook();
  }
  return acted;
}

bool beforeScreenChange(const uint32_t endTimeoutMs) { return detail::suspend(endTimeoutMs); }

bool beforeSleep(const uint32_t timeoutMs) {
  const uint32_t started = port::nowMs();
  while (!detail::suspend(0)) {
    if (port::nowMs() - started >= timeoutMs) return false;
    port::sleepMs(20);
  }
  return true;
}

BuildRelease beforeChapterBuild() {
  if (!config->enabled || radioIdleStopped.load(std::memory_order_relaxed)) return BuildRelease::NotHeld;
  // Never stop a start that is still in flight: its task owns the NimBLE discovery, and a
  // cancel here leaves its callbacks pointing at a task that no longer exists. Wait for it to
  // settle; a start still waiting for the render lock (held by this very build) gives up.
  const uint32_t started = port::nowMs();
  buildWaiting.store(true, std::memory_order_release);
  while (attemptInFlight.load(std::memory_order_acquire) && port::nowMs() - started < kBuildReleaseTimeoutMs) {
    port::sleepMs(20);
  }
  buildWaiting.store(false, std::memory_order_release);
  if (attemptInFlight.load(std::memory_order_acquire)) return BuildRelease::NotHeld;
  heldForBuild.store(true, std::memory_order_relaxed);
  const Heap heap = host->heap();
  say(false, "Section build starved of heap; stopping the radio until the page is shown free=%u largest=%u\n",
      static_cast<unsigned>(heap.freeBytes), static_cast<unsigned>(heap.largestBlock));
  // The stop has its own timeout: a start that settled late must not leave it none.
  const uint32_t stopStarted = port::nowMs();
  bool stopped = detail::stopForIdle();
  while (!stopped && port::nowMs() - stopStarted < kBuildReleaseTimeoutMs) {
    port::sleepMs(20);
    stopped = detail::stopForIdle();
  }
  // A radio still up keeps its heap: another try at the build would starve again.
  if (!stopped) say(true, "Radio did not stop for the section build\n");
  return stopped ? BuildRelease::Released : BuildRelease::StillUp;
}

void afterPaint() {
  heldForBuild.store(false, std::memory_order_relaxed);
  rearmRequested.store(true, std::memory_order_relaxed);
}

bool holdsHeap() {
  // An enabled preference alone is configuration: only a radio that owns or is acquiring its
  // memory, or a book start refused for memory, keeps the book's background work parked.
  return config->enabled && (busy() || readerStartDeferred.load(std::memory_order_relaxed));
}

Status status() {
  Status st{};
  st.compiledIn = port::compiledIn();
  st.running = port::running();
  st.stopping = port::stopping();
  st.starting = attemptInFlight.load(std::memory_order_acquire);
  st.scanning = port::scanning();
  st.connected = port::connected();
  st.connecting = port::connecting();
  st.idleStopped = radioIdleStopped.load(std::memory_order_relaxed);
  st.readerDeferred = readerStartDeferred.load(std::memory_order_relaxed);
  return st;
}

Why why() {
  const bool running = port::running();
  RadioInputs in = inputsFor(running ? Phase::Running : Phase::Idle, lastScene);
  in.linked = port::connected();
  return radioVerdict(in);
}

LinkNote linkNote() { return linkNoteNow.load(std::memory_order_relaxed); }

void acknowledgeLinkNote() { stepLinkNote(false, true); }

bool switchOn() { return startSync(); }

bool switchOff() {
  radioIdleStopped.store(false, std::memory_order_relaxed);
  readerStartDeferred.store(false, std::memory_order_relaxed);
  return detail::suspend(1000);
}

bool service() {
  port::poll();
  if (pairing.addr[0] == '\0' || !port::connected() ||
      strncmp(port::linked().addr, pairing.addr, sizeof(pairing.addr)) != 0) {
    return false;
  }
  memcpy(config->peerAddr, pairing.addr, sizeof(config->peerAddr));
  memcpy(config->peerName, pairing.name, sizeof(config->peerName));
  pairing = Pairing{};
  return true;
}

void scan(const uint32_t durationMs) { port::scan(durationMs); }

bool pair(const char* addr, const char* name) {
  // Only a candidate until it links: a pairing that fails must not replace a bonded choice with
  // an address the stack cannot reconnect (the reader takes presses from the chosen remote only).
  pairing = Pairing{};
  if (!config->enabled || !switchOn() || !port::connect(addr)) return false;
  strncpy(pairing.addr, addr, sizeof(pairing.addr) - 1);
  strncpy(pairing.name, name, sizeof(pairing.name) - 1);
  return true;
}

void disconnect() { port::disconnect(); }

bool forget(const char* addr) {
  port::forget(addr);
  // Forgetting a device forgets its button table too.
  bool changed = forgetRemote(config->remotes, config->remoteCount, addr);
  // A forgotten address left as the choice would be a reconnect into nothing next time.
  if (strncmp(config->peerAddr, addr, sizeof(config->peerAddr)) == 0) {
    config->peerAddr[0] = '\0';
    config->peerName[0] = '\0';
    changed = true;
  }
  return changed;
}

uint8_t bondCount() { return port::bondCount(); }
Peer bond(const uint8_t index) { return port::bond(index); }
uint8_t foundCount() { return port::foundCount(); }
Peer found(const uint8_t index) { return port::found(index); }
Peer linked() { return port::linked(); }
bool takeConnectFailure(char* out, const size_t outLen) { return port::takeConnectFailure(out, outLen); }

bool pollEvent(Event& event) {
  port::KeyPress key;
  while (port::popKey(key)) {
  }
  port::RawEdge raw;
  if (!port::popRaw(raw)) return false;
  event = Event{raw.code(), raw.pressed, raw.atMs, raw.wasRest};
  return true;
}

#ifdef BLE_PAGE_TURNER_PROBE
void injectFrame(const uint8_t* frame, const size_t len) {
  if (!port::connected()) {
    router.table = defaultTableFor("Free3");
    router.chosen = true;
  }
  if (len > 0) port::inject(frame, len);
}
unsigned rawOverflows() { return port::rawOverflows(); }
#endif
#ifdef BLE_PAGE_TURNER_STACK_PROBE
uint32_t startStackLeft() { return port::startStackLeft(); }
#endif

}  // namespace bleturner
