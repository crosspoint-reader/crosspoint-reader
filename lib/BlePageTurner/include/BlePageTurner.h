#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>

#include "BleKeyBinding.h"

// Bluetooth page turner: a BLE remote turns the pages of the book in front.
//
// The host hands the module a Host (what the module may ask of it) and the saved Config
// once, then calls tick() once per main-loop pass and reports the events below. The module
// owns the radio (the SDK's BleKeyboardHost); nothing else in the host starts or stops it.
//
// Threads:
//   - begin, tick, beforeScreenChange, beforeSleep, afterPaint, the settings calls and
//     Host::deliver, Host::yieldForRadio, Host::restartIntoBook run on the host's main loop.
//     tick reads the clock itself (RadioPort::nowMs): a remote's hold is timed against the
//     moment its frame arrived, which can be later than the start of the pass.
//   - beforeChapterBuild runs on the host's render task while it holds its render lock.
//     It may wait (at most 3 s) for a radio start that is already past its cache release,
//     never for one that still needs the render lock: a start waiting for that lock gives
//     up its turn as soon as a chapter build asks for the heap.
//   - A radio start runs on its own short-lived task. There it calls Host::heap,
//     Host::releaseCaches (which only ever TRIES the render lock), Host::fileTransferActive,
//     Host::holdFullSpeed and Host::log.
//   - holdsHeap and status may be called from any task.
namespace bleturner {

struct Heap {
  size_t freeBytes;
  size_t largestBlock;
};

// The saved settings. ConfigJson reads and writes them under the keys blePageTurnerEnabled,
// blePeerAddr, blePeerName, blePrevKeyUsage, bleNextKeyUsage and bleRemotes.
struct Config {
  uint8_t enabled = 0;
  // The remote the user chose: reconnected to without a new scan.
  char peerAddr[18] = "";
  char peerName[32] = "";
  // HID usage learned for each direction from the decoded key path; 0 = not learned.
  uint8_t prevKeyUsage = 0;
  uint8_t nextKeyUsage = 0;
  // Per-remote button tables (BleKeyBinding.h).
  RemoteTable remotes[kMaxRemotes] = {};
  uint8_t remoteCount = 0;
};

// HID usages of the decoded key path. Keyboard page (0x07): Left 0x50, Right 0x4F, Page Up
// 0x4B, Page Down 0x4E, Up 0x52, Down 0x51, Space 0x2C, Enter 0x28, Backspace 0x2A. Volume
// Up/Down and Scan Next/Prev live on the Consumer page (0x0C) and arrive as the low byte of
// the 16-bit usage.
inline constexpr uint8_t kUsageNone = 0;
inline constexpr uint8_t kUsageLeft = 0x50;
inline constexpr uint8_t kUsageRight = 0x4F;
inline constexpr uint8_t kUsagePageUp = 0x4B;
inline constexpr uint8_t kUsagePageDown = 0x4E;
inline constexpr uint8_t kUsageUp = 0x52;
inline constexpr uint8_t kUsageDown = 0x51;
inline constexpr uint8_t kUsageSpace = 0x2C;
inline constexpr uint8_t kUsageEnter = 0x28;
inline constexpr uint8_t kUsageBackspace = 0x2A;
inline constexpr uint8_t kUsageVolumeUp = 0xE9;
inline constexpr uint8_t kUsageVolumeDown = 0xEA;
inline constexpr uint8_t kUsageScanNext = 0xB5;
inline constexpr uint8_t kUsageScanPrev = 0xB6;

// What a decoded key turns: NextPage, PrevPage or None. A learned key replaces the default
// keys of its direction, so a wrong mapping can always be undone; with nothing learned the
// wide default set applies, because cheap remotes send only arrows, Space, Enter or the
// volume keys. Plain typing and any key with a modifier turn nothing.
constexpr Action pageActionFor(const Config& c, const uint8_t usage, const uint8_t mods) {
  if (!c.enabled || usage == kUsageNone || mods != 0) return Action::None;
  if (c.prevKeyUsage != kUsageNone && usage == c.prevKeyUsage) return Action::PrevPage;
  if (c.nextKeyUsage != kUsageNone && usage == c.nextKeyUsage) return Action::NextPage;
  if (c.prevKeyUsage == kUsageNone &&
      (usage == kUsageLeft || usage == kUsagePageUp || usage == kUsageUp || usage == kUsageBackspace ||
       usage == kUsageVolumeDown || usage == kUsageScanPrev)) {
    return Action::PrevPage;
  }
  if (c.nextKeyUsage == kUsageNone &&
      (usage == kUsageRight || usage == kUsagePageDown || usage == kUsageDown || usage == kUsageSpace ||
       usage == kUsageEnter || usage == kUsageVolumeUp || usage == kUsageScanNext)) {
    return Action::NextPage;
  }
  return Action::None;
}

// Why the radio may not be on (Ok: it may). radioVerdict (RadioPolicy.h) is the one place
// that answers; the runtime, the logs and status() read its answer.
enum class Why : uint8_t {
  Ok,
  Off,             // the user turned the page turner off
  NotReading,      // no book in front
  PageNotShown,    // the book has not painted its page yet
  Indexing,        // the book still builds its index and needs the heap
  BuildNeedsHeap,  // a starved chapter build stopped the radio until its page is shown
  StorageBusy,     // USB drive or file transfer owns the card
  IdleNoLink,      // nothing connected for kIdleOffMs
  HeapLow,         // not enough heap for the stack
  HeapInPieces,    // enough in total, but no block the stack can take
};

// Module -> host. Every pointer is required except heapMap.
struct Host {
  Heap (*heap)();
  // Frees rebuildable caches (fonts from the card) under the host's render lock. Only
  // TRIES the lock: false when it is busy, and the start tries again 10 ms later.
  bool (*releaseCaches)();
  // A remote press for the book in front: NextPage, PrevPage, NextChapter, PrevChapter,
  // ReaderMenu or SaveQuote. False when the book did not take it. The two shortcuts count as
  // user activity whatever the answer.
  bool (*deliver)(Action);
  // Asked right before the radio starts on a shown page: the book frees what it can build
  // again (a live layout parser), so the stack does not split the heap around it. False
  // while it cannot yet; the start waits for a later pass.
  bool (*yieldForRadio)();
  // A file transfer owns the radio's heap and the card.
  bool (*fileTransferActive)();
  // The heap is in pieces (shouldRestart): save the reading place and restart into the book.
  void (*restartIntoBook)();
  // Keep the CPU at full speed while the radio is up or changing state.
  void (*holdFullSpeed)(bool hold);
  // One log line; `format` ends with "\n".
  void (*log)(bool error, const char* format, va_list args);
  // Probe builds: print the heap block by block under `tag`. May be null.
  void (*heapMap)(const char* tag);
};

enum class Where : uint8_t { Elsewhere, Reader, PairingScreen };

// Host -> module, once per main-loop pass.
struct Scene {
  Where where;
  // Changes whenever the screen in front changes (a new visit of the book).
  uint32_t visit;
  bool pageShown;     // the book in front has painted its page
  bool bookIndexing;  // the book still builds its index in the background
  bool storageBusy;   // USB drive or file transfer owns the card
  bool wifiOn;
  bool sleeping;  // on the way to sleep
  bool localKey;  // a page key (or touch) of the device was released this pass
};

void begin(const Host& host, Config& config);
// Returns true when a remote press acted this pass (the host counts it as user activity).
bool tick(const Scene& scene);
// False: the radio is still stopping (or a start is in flight); call again next pass before
// changing screens. endTimeoutMs: how long the SDK may wait for the stack to stop this call.
bool beforeScreenChange(uint32_t endTimeoutMs = 0);
// Stops the radio before sleep, waiting up to timeoutMs. False: it did not stop in time.
bool beforeSleep(uint32_t timeoutMs);
// A chapter build ran out of heap. Released: the radio stopped until afterPaint(), build
// again. StillUp: it was asked to stop and did not within 3 s. NotHeld: the radio is not
// the module's to give (off, idle, or a start still settling).
enum class BuildRelease : uint8_t { NotHeld, Released, StillUp };
BuildRelease beforeChapterBuild();
// Call once, when the page of a build that got Released or StillUp is on screen: the radio
// stopped for it may start again.
void afterPaint();
// The radio holds the heap or is asking for it: the book defers its background work.
bool holdsHeap();

struct Status {
  bool compiledIn;
  bool running;
  bool stopping;
  bool starting;
  bool scanning;
  bool connected;
  bool connecting;
  bool idleStopped;     // stopped for idleness, until a key or the user starts it again
  bool readerDeferred;  // the book's start was refused for memory
};
Status status();
// Why the radio may not be on at the last tick (Ok: it may), from radioVerdict.
Why why();

// What the book's status bar says about the remote in place of its title. Connecting from the
// moment a book is entered until a remote links; Failed when the radio was refused for this
// visit or ran kLinkNoteFailMs without a link, until the next page turn; None otherwise.
// nextLinkNote (RadioPolicy.h) decides; tick() and acknowledgeLinkNote() apply it.
enum class LinkNote : uint8_t { None, Connecting, Failed };
// The note for the book in front. Any task.
LinkNote linkNote();
// A page turn was applied in the book: a Failed note gives the title back. Main loop.
void acknowledgeLinkNote();

// --- Settings screen -------------------------------------------------------------
struct Peer {
  const char* addr;
  const char* name;
};
// One start now, on the calling task (the user asked for it). Clears the idle and memory
// reasons first.
bool switchOn();
// The user turned it off: clears the reasons and stops, waiting up to 1 s.
bool switchOff();
// Connection work while the settings screen is open. True when it changed the Config (a remote
// being paired linked and became the chosen one): the host saves it.
bool service();
// Scans for durationMs; 0 stops a scan.
void scan(uint32_t durationMs);
// Pairs (or connects) this remote. It becomes the chosen one only once it links (service()); a
// pairing that fails leaves the choice as it was.
bool pair(const char* addr, const char* name);
void disconnect();
// Forgets the bond, its button table and, if it was the chosen remote, the choice.
// True when the Config changed (the host saves it).
bool forget(const char* addr);
uint8_t bondCount();
Peer bond(uint8_t index);
uint8_t foundCount();
Peer found(uint8_t index);
Peer linked();
bool takeConnectFailure(char* out, size_t outLen);

// One raw edge of the linked remote. Key events are drained on the way: while the settings
// screen is open the remote's input belongs to it.
struct Event {
  uint32_t code;  // BleKeyBinding.h button code
  bool pressed;
  uint32_t atMs;
  bool wasRest;  // a release showing the press was the remote's rest frame
};
bool pollEvent(Event& event);

// Measurement builds only. The host defines BLE_PAGE_TURNER_PROBE for the injection hook and the
// radio's heap log lines, BLE_PAGE_TURNER_STACK_PROBE for the start task's stack figure.
#ifdef BLE_PAGE_TURNER_PROBE
// One HID frame through the real ingest path, as if the remote had sent it (len 0: none).
// With nothing linked the next edges go through the built-in three-button table.
void injectFrame(const uint8_t* frame, size_t len);
unsigned rawOverflows();
#endif
#ifdef BLE_PAGE_TURNER_STACK_PROBE
uint32_t startStackLeft();
#endif

}  // namespace bleturner
