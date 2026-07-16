// BleSyncManager — background multi-book reconcile engine (PROTOCOL-v2.md §4).
//
// The reconcile logic that used to live inside BleSyncTestActivity, lifted into a
// singleton the MAIN LOOP pumps. This lets a sync run in the background while the
// library (or a blocking wait screen) is the visible activity: the UI just reads
// status() and draws a small indicator — it doesn't own the sync.
//
// Zero cost when idle: loop() returns immediately unless a sync is active, and a
// sync only starts on an explicit trigger (all gated on getBleSyncEnabled()).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

class GfxRenderer;

namespace BleSync {

enum class Phase : uint8_t {
  Idle,         // nothing running, no result to show
  Advertising,  // radio up, waiting for the phone to connect
  Syncing,      // connected, exchanging books
  Success,      // finished OK (glyph shown briefly)
  Failed,       // finished with an error (details in settings)
};

struct Status {
  Phase phase = Phase::Idle;
  int bookIndex = 0;    // books reconciled so far (sent + applied)
  int bookCount = 0;    // books that need syncing (known after manifest exchange; 0 = unknown yet)
  int percent = 0;      // 0..100 overall (bookIndex / bookCount)
  uint8_t spin = 0;     // spinner step — advances one notch per state change
  bool result = false;  // last finished run succeeded
  std::string error;    // human-readable failure reason (shown in settings)
};

// The one manager. Renderer is needed for applyRemote's page mapping.
class Manager {
 public:
  static Manager& instance();

  // Begin a background reconcile. `deadlineMs` bounds the whole run; `blocking`
  // marks a book-open/boot-to-book wait (affects only how the UI treats it).
  // No-op if a sync is already active or BLE sync is disabled.
  // Returns true only when a fresh run was accepted. Callers that queue sync
  // work must not consume their request when this returns false.
  bool start(GfxRenderer& renderer, unsigned long deadlineMs, bool blocking);

  // Pump the state machine. Cheap no-op when idle. Call every main-loop iteration.
  void loop();

  // Stop + release the radio now (e.g. user skipped, or Wi-Fi is needed).
  void stop();

  bool isActive() const;    // Advertising or Syncing
  bool isBlocking() const;  // active AND started as a book-open wait
  Status status() const;    // snapshot for the indicator / wait screen
  bool statusDirty();       // true once per state change (drives a re-render); clears the flag

  // True while a finished glyph (✓ / ✗) should still be shown, then goes false.
  bool hasFreshResult() const;

  // millis() of the last SUCCESSFUL sync (0 = none this boot). Freshness guard:
  // skip the book-open wait when a sync completed very recently.
  unsigned long lastSuccessMs() const;

  const std::string& lastError() const;  // persists after the run for the settings page

 private:
  Manager() = default;
};

// True when opening a book should first run a BLOCKING sync (PROTOCOL-v2.md §5):
// BLE Sync on and either a reconcile is already active (wait for it) or no
// successful sync completed very recently.
bool shouldSyncBeforeOpen();

}  // namespace BleSync

// Convenience accessor, mirrors KOREADER_STORE / APP_STATE style.
#define BLE_SYNC BleSync::Manager::instance()
