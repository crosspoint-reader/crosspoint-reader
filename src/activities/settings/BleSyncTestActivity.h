// BleSyncTestActivity — "BLE Sync / Pair" visible-sync screen (Protocol v2).
//
// Advertises for the phone, then runs a MULTI-BOOK reconcile (PROTOCOL-v2.md §4):
//   1. push the open book's PROGRESS once (v1-compatible fast path),
//   2. send our MANIFEST of recent books (resent until the phone replies),
//   3. on the phone's MANIFEST, push PROGRESS for every book we're newer on,
//   4. apply every PROGRESS the phone pushes (newest-wins, any recent book).
// Manifests/pushes are resent until the phone replies — the first notify usually
// fires before the central has finished subscribing, so a single send is lost.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"

class BleSyncTestActivity : public Activity {
 public:
  // autoExitMs > 0: auto-finish after that many ms (or shortly after the sync
  // settles) — used by the boot/exit auto-triggers. 0 = stay until Back.
  explicit BleSyncTestActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, unsigned long autoExitMs = 0);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }

 private:
  enum class State { Advertising, Connected };

  void startAdvertising();
  void armReconcile();                                 // reset per-connection state
  void pushProgressFor(const std::string& titleHash);  // send one book's PROGRESS (deduped)

  State state_ = State::Advertising;

  bool everConnected_ = false;  // a phone connected at least once this screen session

  // Reconcile state (re-armed on each connection).
  bool pushedOpen_ = false;        // v1 fast-path push of the open book done
  bool sentManifest_ = false;      // our MANIFEST sent at least once
  bool gotPhoneManifest_ = false;  // phone replied with its MANIFEST
  unsigned long lastManifestMs_ = 0;
  unsigned long lastActivityMs_ = 0;
  std::vector<std::string> pushedKeys_;  // title_hashes we've already pushed
  int booksSent_ = 0;
  int booksApplied_ = 0;

  // Auto-exit (boot/exit triggers).
  unsigned long autoExitMs_ = 0;
  unsigned long enterMs_ = 0;

  std::string statusSummary_;
};
