#pragma once

#include <I18n.h>
#include <Memory.h>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct SubstackEpub {
  char id[64];
  char title[128];
  char profileName[64];
  size_t epubSize;
};

/**
 * SubstackActivity
 *
 * Lists EPUBs pending download from the CrossInk API and handles downloading
 * them to the SD card under /substacks/<epub_id>.epub.
 *
 * State machine:
 *   PAIRING     → not configured yet: requested a pairing code, showing its
 *                 QR, polling for the dashboard to claim it. If the initial
 *                 GET /pair/new call itself fails (no Wi-Fi, API down), that
 *                 goes to ERROR like any other fetch failure — there's no
 *                 separate "not configured" state anymore.
 *   FETCHING    → HTTP GET /device/:id/pending in flight
 *   EMPTY       → no pending EPUBs
 *   LIST        → showing the list, user can select to open/download
 *   DOWNLOADING → download in progress, shows percentage
 *   ERROR       → network or parse failure, errorMsg shown
 */
class SubstackActivity final : public Activity {
 public:
  explicit SubstackActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Substack", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  void onExit() override;

 private:
  enum class State { PAIRING, FETCHING, EMPTY, LIST, DOWNLOADING, ERROR };

  State state = State::FETCHING;
  int selectorIndex = 0;
  int downloadProgress = 0;
  char errorMsg[128] = "";

  static constexpr size_t MAX_EPUBS = 20;
  SubstackEpub epubs[MAX_EPUBS];
  size_t epubCount = 0;

  // Handle for the active background task (fetch, download, or pairing poll)
  TaskHandle_t networkTask = nullptr;

  // Fetch task — GET /device/:id/pending
  static void networkTaskTrampoline(void* param);
  void runNetworkTask();

  // Download task — GET /device/:id/epub/:epub_id → SD card
  static void downloadTaskTrampoline(void* param);
  void runDownloadTask();

  // Open the selected EPUB (download first if not on SD)
  void openSelected();

  void buildPendingUrl(char* buf, size_t bufLen) const;
  void buildEpubUrl(char* buf, size_t bufLen, const char* epubId) const;
  bool parsePendingResponse(const char* json, size_t len);

  // ── Pairing (no account context yet — see PAIRING state above) ──────────────
  char pairCode[16] = "";
  char pairUrl[256] = "";
  char pairApiUrl[128] = "";  // apiUrl this pairing attempt is bound to
  // Checked by runPairingTask() between polls so onExit() can stop it
  // without waiting the full poll interval out.
  volatile bool pairingCancelled = false;

  // Requests a pairing code (GET /pair/new); on success fills pairCode/
  // pairUrl and returns true. Runs on the calling thread (onEnter) — it's a
  // single short request, unlike the polling loop that follows it.
  bool requestPairingCode();
  static void pairingTaskTrampoline(void* param);
  // Polls GET /pair/:code until claimed, cancelled, or the device reboots.
  // On success, persists apiUrl/deviceId/apiToken to SUBSTACK_STORE.
  void runPairingTask();

  ButtonNavigator buttonNavigator;
};
