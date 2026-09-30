#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

// Binding EACH button of a BLE remote. A button is known by the fingerprint of its
// RAW HID frame (report id, byte index, value; see freeink::RawButtonEvent). The
// decoded usage is too lossy for that: the decoder drops the second byte of the
// three-button remote and cuts 16-bit usages to 8 bits. Each remote (by BLE address) has one table of at
// most 8 slots, saved in settings.json and applied again on reconnect.
//
// Pure logic, no SDK or settings dependency: the module's runtime, the host's settings
// screen and the tests call these same functions, so each rule is stated once.
namespace bleturner {

// One wait for a button lasts at most 15 seconds, as the screen tells the user.
inline constexpr uint32_t kWaitMs = 15000;
// Held this long or more is a "hold". A host whose own buttons skip chapters on a
// hold should use the same number and pin it with a static_assert.
inline constexpr uint32_t kHoldMs = 700;
// While learning: how long to wait for the pressed button's release. No release
// means the remote does not report it, and the button is learned as a tap.
inline constexpr uint32_t kReleaseWaitMs = 2000;

// Stored by value in settings.json (bits 28-31 of a slot): new actions go last, and the
// numbers never change (a test pins them). The four moves are the remote's own;
// ReaderMenu and SaveQuote are handed to the host, which runs its own action of that name.
enum class Action : uint8_t { None, NextPage, PrevPage, NextChapter, PrevChapter, ReaderMenu, SaveQuote };

// One slot: bits 0-23 the button code (value | byte index << 8 | report id << 16),
// bit 24 "hold", bit 25 "any report id" (built-in defaults only, since that remote's
// report id has not been measured), bits 28-31 the action.
using Binding = uint32_t;
inline constexpr uint32_t kCodeMask = 0x00FFFFFFu;
inline constexpr uint32_t kHoldBit = 1u << 24;
inline constexpr uint32_t kAnyReportBit = 1u << 25;
inline constexpr uint8_t kActionShift = 28;

inline constexpr uint8_t kMaxBindings = 8;  // 4 buttons x tap/hold
inline constexpr uint8_t kMaxRemotes = 4;   // = BleKeyboardHost::kMaxBonds

struct RemoteTable {
  char addr[18];
  uint8_t count;
  Binding bindings[kMaxBindings];
};

constexpr Binding makeBinding(const uint32_t code, const bool hold, const Action action) {
  return (code & kCodeMask) | (hold ? kHoldBit : 0u) | static_cast<uint32_t>(action) << kActionShift;
}
constexpr Action actionOf(const Binding b) { return static_cast<Action>(b >> kActionShift); }

// A slot read from the card is kept only when every bit means something: an action
// in range, a non-zero value, no unknown bit. Garbage is dropped rather than turned
// into a button that does something odd.
constexpr bool valid(const Binding b) {
  return (b & ~(kCodeMask | kHoldBit | kAnyReportBit | 0xF0000000u)) == 0 && (b & 0xFFu) != 0 &&
         actionOf(b) != Action::None && static_cast<uint8_t>(actionOf(b)) <= static_cast<uint8_t>(Action::SaveQuote);
}

constexpr bool matches(const Binding b, const uint32_t code, const bool hold) {
  const uint32_t mask = (b & kAnyReportBit) ? 0x0100FFFFu : 0x01FFFFFFu;
  return ((b ^ (code | (hold ? kHoldBit : 0u))) & mask) == 0;
}

// Lookup: at most 8 compares over 32 contiguous bytes, no allocation.
inline Action lookup(const RemoteTable& t, const uint32_t code, const bool hold) {
  for (uint8_t i = 0; i < t.count; ++i) {
    if (matches(t.bindings[i], code, hold)) return actionOf(t.bindings[i]);
  }
  return Action::None;
}

inline bool find(const RemoteTable& t, const Action action, Binding& out) {
  for (uint8_t i = 0; i < t.count; ++i) {
    if (actionOf(t.bindings[i]) == action) {
      out = t.bindings[i];
      return true;
    }
  }
  return false;
}

inline const char* actionName(const Action action) {
  static const char* const kNames[] = {"none",         "next",        "prev",      "next_chapter",
                                       "prev_chapter", "reader_menu", "save_quote"};
  return kNames[static_cast<uint8_t>(action) <= 6 ? static_cast<uint8_t>(action) : 0];
}

// "3:1=02" (report id 3, byte 1, value 0x02); "1=02" for an any-report default slot;
// "0x43" for a button known only by the key the decoder read (byte index 0xFF).
inline void formatCode(char* out, const size_t n, const Binding b) {
  if (((b >> 8) & 0xFF) == 0xFF) {
    snprintf(out, n, "0x%02X", static_cast<unsigned>(b & 0xFF));
  } else if (b & kAnyReportBit) {
    snprintf(out, n, "%u=%02X", static_cast<unsigned>((b >> 8) & 0xFF), static_cast<unsigned>(b & 0xFF));
  } else {
    snprintf(out, n, "%u:%u=%02X", static_cast<unsigned>((b >> 16) & 0xFF), static_cast<unsigned>((b >> 8) & 0xFF),
             static_cast<unsigned>(b & 0xFF));
  }
}

// --- Built-in default ----------------------------------------------------------
// The three-button remote named "Free3": button 3 sends "00 02 00" on a tap (byte 1)
// and "08 00 00" when held (byte 0). Default: tap = next chapter, hold = previous
// chapter. The two page buttons are NOT here: they keep the old usage mapping exactly
// as before, so what the user bound them to is not overridden. This is data only; the
// lookup knows nothing about this remote.
constexpr RemoteTable kThreeButtonDefault = {"",
                                             2,
                                             {makeBinding(0x000102, false, Action::NextChapter) | kAnyReportBit,
                                              makeBinding(0x000008, false, Action::PrevChapter) | kAnyReportBit}};

inline const RemoteTable* defaultTableFor(const char* name) {
  return name != nullptr && strncmp(name, "Free3", 5) == 0 ? &kThreeButtonDefault : nullptr;
}

// Table of the connected remote: the saved one for its address, else the default for
// its name, else none (nullptr = the old usage path, exactly as today). Writes nothing.
inline const RemoteTable* tableFor(const RemoteTable* tables, const uint8_t count, const char* addr, const char* name) {
  if (addr == nullptr || addr[0] == '\0') return nullptr;
  for (uint8_t i = 0; i < count; ++i) {
    if (strncmp(tables[i].addr, addr, sizeof(tables[i].addr)) == 0) return &tables[i];
  }
  return defaultTableFor(name);
}

// Only a table with at least one slot changes the path. An empty table (cleared by
// the user) keeps its address so the default does not come back, and takes the old path.
inline bool routes(const RemoteTable* t) { return t != nullptr && t->count > 0; }

// Editable table of a remote: the saved one, or a new one copied from its default.
// nullptr when kMaxRemotes remotes already have one.
inline RemoteTable* editableTable(RemoteTable* tables, uint8_t& count, const char* addr, const char* name) {
  if (addr == nullptr || addr[0] == '\0') return nullptr;
  for (uint8_t i = 0; i < count; ++i) {
    if (strncmp(tables[i].addr, addr, sizeof(tables[i].addr)) == 0) return &tables[i];
  }
  if (count >= kMaxRemotes) return nullptr;
  RemoteTable& t = tables[count++];
  const RemoteTable* seed = defaultTableFor(name);
  t = seed != nullptr ? *seed : RemoteTable{};
  strncpy(t.addr, addr, sizeof(t.addr) - 1);
  t.addr[sizeof(t.addr) - 1] = '\0';
  return &t;
}

// Forgetting a device forgets its table too.
inline bool forgetRemote(RemoteTable* tables, uint8_t& count, const char* addr) {
  for (uint8_t i = 0; i < count; ++i) {
    if (strncmp(tables[i].addr, addr, sizeof(tables[i].addr)) != 0) continue;
    for (uint8_t j = i + 1; j < count; ++j) tables[j - 1] = tables[j];
    --count;
    return true;
  }
  return false;
}

inline bool clearAction(RemoteTable& t, const Action action) {
  uint8_t kept = 0;
  for (uint8_t i = 0; i < t.count; ++i) {
    if (actionOf(t.bindings[i]) != action) t.bindings[kept++] = t.bindings[i];
  }
  const bool changed = kept != t.count;
  t.count = kept;
  return changed;
}

// Store a learned button. An action has one button and a gesture (button + tap or
// hold) does one thing: the old slots of both go. false when the table is full,
// never a silent overwrite.
inline bool learn(RemoteTable& t, const Action action, const uint32_t code, const bool hold) {
  clearAction(t, action);
  uint8_t kept = 0;
  for (uint8_t i = 0; i < t.count; ++i) {
    if (!matches(t.bindings[i], code, hold)) t.bindings[kept++] = t.bindings[i];
  }
  t.count = kept;
  if (t.count >= kMaxBindings) return false;
  t.bindings[t.count++] = makeBinding(code, hold, action);
  return true;
}

// --- Decision for each raw edge ------------------------------------------------

// A press still waiting to become a tap or a hold. Only for a button WITH a hold slot.
struct HoldWait {
  uint32_t code = 0;
  uint32_t atMs = 0;
  Action tap = Action::None;
  Action hold = Action::None;
  bool active = false;
};

// The ONE decision for a raw edge of a remote with a table. `legacy` is what the old
// usage mapping gives the key read from the same frame: a button the table does not
// name keeps turning pages the way it always did.
//   - Press, no hold slot for the button: act now (page turns stay at 1-2 ms).
//   - Press, the button has a hold slot: wait; a release before kHoldMs is a tap,
//     later is a hold.
//   - Release: only ends a wait.
inline Action onRawEdge(const RemoteTable& t, const uint32_t code, const bool pressed, const uint32_t atMs,
                        const Action legacy, HoldWait& w) {
  if (pressed) {
    const Action tap = lookup(t, code, false);
    const Action now = tap != Action::None ? tap : legacy;
    const Action hold = lookup(t, code, true);
    if (hold == Action::None) return now;
    w = HoldWait{code, atMs, now, hold, true};
    return Action::None;
  }
  if (!w.active || w.code != code) return Action::None;
  w.active = false;
  return atMs - w.atMs < kHoldMs ? w.tap : w.hold;
}

// Called every pass: a waiting button still down after kHoldMs is a hold, acted on
// now without waiting for the release.
inline Action pollHold(HoldWait& w, const uint32_t nowMs) {
  if (!w.active || nowMs - w.atMs < kHoldMs) return Action::None;
  w.active = false;
  return w.hold;
}

// Tick state: the table is picked ONCE per link (off the hot path) and forgotten
// when the link changes or the reader leaves the front.
struct Router {
  const RemoteTable* table = nullptr;
  bool chosen = false;
  bool linked = false;
  HoldWait wait;

  void follow(const bool isLinked) {
    if (isLinked == linked) return;
    *this = Router();
    linked = isLinked;
  }
};

}  // namespace bleturner
