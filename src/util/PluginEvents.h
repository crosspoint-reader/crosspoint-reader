#pragma once

#include <cstddef>
#include <cstdint>

class GfxRenderer;

// Firmware event hooks for SD plugins.
//
// The firmware emits a small whitelisted set of events; a plugin's device.json
// subscribes by declaring an "events" section whose handlers are the same
// declarative request objects the catalog vocabulary uses:
//
//   "events": {
//     "reader.exit": {
//       "request": { "method": "POST", "url": "https://...", "headers": {...},
//                    "body": "{\"book\":\"{event.book}\",\"pct\":{event.percent}}" },
//       "toast": "Synced {event.book}"
//     },
//     "sleep.enter": {
//       "download": { "url": "https://.../daily.bmp", "dest": "/sleep.bmp" }
//     }
//   }
//
// A handler is either a "request" (response discarded; an acknowledgement) or
// a "download" (response streamed to `dest` on SD, e.g. a sleep image).
//
// Execution is deferred: plugin handlers need the network, and events mostly
// fire with WiFi down (leaving the reader, entering sleep). emit() appends one
// JSON line to the plugin's SD outbox (<plugin dir>/events.jsonl) and costs
// nothing beyond that; drain() later replays queued events through the
// declared requests whenever the device is already online. Substitution in
// url/body/headers/toast: {token}, {cfg.KEY}, and the event's {event.*} vars.
//
// sleep.enter exists to act before the chip powers down (a fresh sleep image,
// a pre-sleep progress push), so subscribing implies "connect": true: the
// sleep path brings WiFi up to deliver it at sleep entry rather than waiting
// for the next online session. Delivery stays at-least-once — a failed
// sleep-time drain leaves the events queued for the next drain.
//
// Whitelist only: unknown names in a manifest are ignored with a log line, so
// manifests written against newer firmware degrade gracefully. Event names are
// a compatibility promise (semantic, not activity class names).
namespace pluginevents {

enum class Event : uint8_t {
  ReaderOpen,      // "reader.open"      vars: book
  ReaderExit,      // "reader.exit"      vars: book, percent
  ReaderSession,   // "reader.session"   vars: book, document, active-session summary
  BookDownloaded,  // "book.downloaded"  vars: path, title, plugin
  SleepEnter,      // "sleep.enter"      vars: book, percent when sleeping from a reader, else none
  COUNT
};

struct Var {
  const char* key;    // name inside {event.*}, e.g. "book"
  const char* value;  // NUL-terminated UTF-8
};

// Re-reads every installed plugin's device.json "events" section into the
// static subscription table. Cheap relative to its callers (a handful of small
// SD reads); call at boot and whenever plugins may have changed (plugin list
// opened, web-server session ended).
void refreshSubscriptions();

// True when at least one plugin subscribes to `e` (a few string compares).
bool anySubscriber(Event e);

// Bit per Event the named plugin subscribes to (0 = none), for disclosing
// what leaves the device. Reflects the last refreshSubscriptions().
uint8_t subscriptionMask(const char* plugin);

// True when any queued event belongs to a handler marked "connect": true
// (sleep.enter subscriptions imply it; see above): the sleep path may then
// bring WiFi up, bounded, so delivery happens before the chip powers down
// (e.g. a fresh sleep image) instead of in the next online session.
bool wantsConnectAny();

// Appends the event to each subscribing plugin's outbox. No-op without
// subscribers. An outbox over the size cap is dropped wholesale first (the
// newest events carry the current state; stale ones are worthless by then).
void emit(Event e, const Var* vars, size_t varCount);

// Replays queued events synchronously while WiFi is connected. maxEvents caps
// delivered lines; failures stay queued. Opt-in limits share caller HTTP
// operations and a cooperative deadline across all plugins, including auth
// mint and retry. Blocking SDK DNS/TCP connect can exceed the deadline.
// A non-null renderer shows the handler's toast after a successful request.
struct DrainLimits {
  size_t maxAttempts;     // 0 disables the operation count limit
  uint32_t maxElapsedMs;  // 0 disables the cooperative deadline
};
void drain(GfxRenderer* renderer, size_t maxEvents = 4);
void drain(GfxRenderer* renderer, size_t maxEvents, DrainLimits limits);

}  // namespace pluginevents
