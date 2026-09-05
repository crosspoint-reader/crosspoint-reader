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
//       "download": { "url": "https://.../daily.bmp", "dest": "/sleep.bmp" },
//       "connect": true
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
// Whitelist only: unknown names in a manifest are ignored with a log line, so
// manifests written against newer firmware degrade gracefully. Event names are
// a compatibility promise (semantic, not activity class names).
namespace pluginevents {

enum class Event : uint8_t {
  ReaderOpen,      // "reader.open"      vars: book
  ReaderExit,      // "reader.exit"      vars: book, percent
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

// True when some subscriber to `e` marks its handler "connect": true AND has
// queued events waiting: the caller may bring WiFi up (bounded, opt-in) so
// delivery happens now instead of the next online session. Used by the sleep
// path to fetch e.g. a fresh sleep image before the chip powers down.
bool wantsConnect(Event e);

// Appends the event to each subscribing plugin's outbox. No-op without
// subscribers. An outbox over the size cap is dropped wholesale first (the
// newest events carry the current state; stale ones are worthless by then).
void emit(Event e, const Var* vars, size_t varCount);

// Replays queued events through their manifest handlers. Call only while WiFi
// is connected; requests run synchronously on the calling task. At most
// `maxEvents` handlers run per call (each is a bounded HTTPS request) so a
// backlog cannot stall an interactive session. Successfully delivered events
// leave the outbox; failures stay for the next drain. When `renderer` is
// non-null, a handler's "toast" template is shown via the standard popup after
// its request succeeds.
void drain(GfxRenderer* renderer, size_t maxEvents = 4);

}  // namespace pluginevents
