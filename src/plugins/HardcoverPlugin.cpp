/**
 * HardcoverPlugin — Hardcover.app reading-activity scrobbler.
 *
 * Syncs reading progress to Hardcover.app via their GraphQL API.
 * All network I/O is deferred to a background FreeRTOS task to keep
 * plugin hooks under the 10 ms return-time budget.
 *
 * Flow:
 *   onBoot   → Load cached book mapping; create background sync task
 *   onBookOpen → Queue a "book opened" event (ISBN lookup + set reading)
 *   onPageTurn → Debounce: queue progress update every N page turns
 *   onBookClose → Queue final progress flush
 *   onSleep  → Queue any pending progress update
 *
 * CONVENTIONS (see .skills/SKILL.md):
 *  - Use LOG_DBG / LOG_INF / LOG_ERR for all output; never raw Serial.
 *  - Keep stack locals under 256 bytes.
 *  - Avoid std::string in hot paths; use const char* or snprintf.
 *  - Mark large constant data `static constexpr` to keep it in Flash.
 *  - Plugin hooks must return quickly — they run on the main loop thread.
 *
 * Disabled by default.  Enable from Settings → Plugins.
 */

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <HardcoverClient.h>
#include <HardcoverCredentialStore.h>
#include <Logging.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "plugin/CprPlugin.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

namespace {
constexpr char LOG_TAG[] = "HCPLG";

// How many page turns between automatic progress syncs
static constexpr int PAGE_TURN_SYNC_INTERVAL = 10;

// FreeRTOS task stack size (bytes) — 8192 to accommodate TLS (WiFiClientSecure),
// HTTP (HTTPClient) and JSON (deserializeJson) stack usage in executeGraphQL().
static constexpr uint32_t SYNC_TASK_STACK = 8192;

// Queue capacity (number of pending commands)
static constexpr int QUEUE_SIZE = 8;

// Maximum time (ms) to wait for the background sync task to acknowledge a
// FLUSH command before entering deep sleep.
static constexpr uint32_t FLUSH_TIMEOUT_MS = 3000;

// Paths on SD card
constexpr char STATE_DIR[] = "/.crosspoint/plugin_hardcover";
constexpr char STATE_FILE[] = "/.crosspoint/plugin_hardcover/state.json";
constexpr char BOOK_MAP_FILE[] = "/.crosspoint/plugin_hardcover/book_map.json";

// ---------------------------------------------------------------------------
// Sync command types queued from hooks → background task
// ---------------------------------------------------------------------------

enum class SyncCmd : uint8_t {
  BOOK_OPEN,   // Look up book, set "Currently Reading"
  PAGE_TURN,   // Update progress percentage
  BOOK_CLOSE,  // Flush final progress
  FLUSH,       // Flush pending (from onSleep)
};

struct SyncMessage {
  SyncCmd cmd;
  char path[256];                   // EPUB path (BOOK_OPEN only) — 256 for long SD card paths
  int chapter;                      // Current chapter
  int page;                         // Current page
  SemaphoreHandle_t completionSem;  // FLUSH only: semaphore to signal completion (nullptr if creation failed or unused)
};

// ---------------------------------------------------------------------------
// Module state (file-scoped statics)
// ---------------------------------------------------------------------------

TaskHandle_t syncTaskHandle = nullptr;
QueueHandle_t syncQueue = nullptr;

// Cached mapping for the currently open book
int currentBookId = 0;
int currentUserBookId = 0;
int pageTurnCounter = 0;
int lastSyncedPage = 0;

// Sentinel value meaning "total page count unknown — do not send progress_percentage"
static constexpr float PROGRESS_UNKNOWN = -1.0f;
float lastProgressPercent = PROGRESS_UNKNOWN;

// ---------------------------------------------------------------------------
// SD persistence helpers
// ---------------------------------------------------------------------------

/// Load the cached book mapping (queryKey → book_id, user_book_id) from SD.
void loadBookMap(const char* queryKey, int& bookId, int& userBookId) {
  bookId = 0;
  userBookId = 0;

  if (!Storage.exists(BOOK_MAP_FILE)) return;

  String json = Storage.readFile(BOOK_MAP_FILE);
  if (json.isEmpty()) return;

  JsonDocument doc;
  if (deserializeJson(doc, json)) return;

  JsonObject entry = doc[queryKey];
  if (entry) {
    bookId = entry["book_id"] | 0;
    userBookId = entry["user_book_id"] | 0;
    LOG_DBG(LOG_TAG, "Cache hit: %s → book=%d ub=%d", queryKey, bookId, userBookId);
  }
}

// Maximum entries in the book_map.json cache.  Once exceeded the oldest
// entries (by insertion order, which is also JsonObject iteration order)
// are evicted before adding the new one.
static constexpr int MAX_BOOK_MAP_ENTRIES = 64;

/// Save a book mapping entry to the SD cache.
void saveBookMap(const char* queryKey, int bookId, int userBookId) {
  Storage.mkdir(STATE_DIR);

  // Load existing map (if any) and merge
  JsonDocument doc;
  if (Storage.exists(BOOK_MAP_FILE)) {
    String json = Storage.readFile(BOOK_MAP_FILE);
    if (!json.isEmpty()) {
      if (deserializeJson(doc, json)) {
        // Corrupt file — start fresh
        doc.clear();
      }
    }
  }

  // Evict oldest entries if at capacity (JsonObject iterates in insertion order)
  JsonObject root = doc.as<JsonObject>();
  while (root.size() >= MAX_BOOK_MAP_ENTRIES) {
    // Remove the first (oldest) key
    auto it = root.begin();
    if (it == root.end()) break;
    root.remove(it);
  }

  JsonObject entry = doc[queryKey].to<JsonObject>();
  entry["book_id"] = bookId;
  entry["user_book_id"] = userBookId;

  String json;
  json.reserve(256);
  serializeJson(doc, json);
  Storage.writeFile(BOOK_MAP_FILE, json);
}

/// Save current sync state (last page, progress) to SD.
void saveSyncState() {
  Storage.mkdir(STATE_DIR);

  JsonDocument doc;
  doc["book_id"] = currentBookId;
  doc["user_book_id"] = currentUserBookId;
  doc["last_synced_page"] = lastSyncedPage;
  doc["progress_percent"] = lastProgressPercent;

  String json;
  json.reserve(128);
  serializeJson(doc, json);
  Storage.writeFile(STATE_FILE, json);
}

/// Load previously saved sync state from SD (called on BOOK_OPEN to resume after reboot).
void loadSyncState() {
  if (!Storage.exists(STATE_FILE)) return;

  String json = Storage.readFile(STATE_FILE);
  if (json.isEmpty()) return;

  JsonDocument doc;
  if (deserializeJson(doc, json)) return;

  currentBookId = doc["book_id"] | 0;
  currentUserBookId = doc["user_book_id"] | 0;
  lastSyncedPage = doc["last_synced_page"] | 0;
  lastProgressPercent = doc["progress_percent"] | PROGRESS_UNKNOWN;
  LOG_DBG(LOG_TAG, "Loaded sync state: book=%d ub=%d page=%d", currentBookId, currentUserBookId, lastSyncedPage);
}

// ---------------------------------------------------------------------------
// ISBN extraction from EPUB path
// ---------------------------------------------------------------------------

/// Extract a search query from the EPUB path.
/// TODO: Parse OPF metadata inside the EPUB to extract actual ISBN for more
///       accurate Hardcover book matching.  Currently uses the filename stem
///       (minus extension) as the search query, which works well for files
///       named with titles but poorly for opaque filenames.
void extractSearchQuery(const char* epubPath, char* outQuery, size_t maxLen) {
  // Find last '/' to get filename
  const char* filename = epubPath;
  const char* slash = strrchr(epubPath, '/');
  if (slash) filename = slash + 1;

  // Copy without extension
  const char* dot = strrchr(filename, '.');
  size_t len = dot ? (size_t)(dot - filename) : strlen(filename);
  if (len >= maxLen) len = maxLen - 1;
  memcpy(outQuery, filename, len);
  outQuery[len] = '\0';
}

// ---------------------------------------------------------------------------
// Background sync task
// ---------------------------------------------------------------------------

void syncTaskFunc(void* /*param*/) {
  SyncMessage msg;

  for (;;) {
    // Block until a message arrives (portMAX_DELAY = wait forever)
    if (xQueueReceive(syncQueue, &msg, portMAX_DELAY) != pdTRUE) {
      continue;
    }

    if (!HARDCOVER_STORE.hasToken()) {
      LOG_DBG(LOG_TAG, "No token — skipping sync");
      // Unblock the sender if this was a FLUSH with a completion semaphore,
      // otherwise hardcoverSleep() will always timeout waiting.
      if (msg.completionSem) {
        xSemaphoreGive(msg.completionSem);
      }
      continue;
    }

    switch (msg.cmd) {
      case SyncCmd::BOOK_OPEN: {
        // Extract search query from EPUB path
        char query[128];
        extractSearchQuery(msg.path, query, sizeof(query));
        LOG_DBG(LOG_TAG, "Book opened, searching: %s", query);

        // Restore persisted sync state from a previous session (e.g. after reboot/deep sleep).
        // Populates lastSyncedPage and lastProgressPercent; currentBookId/currentUserBookId
        // are overridden by loadBookMap() below if the book is already in the cache.
        loadSyncState();

        // Check cached mapping first (authoritative source for currentBookId/currentUserBookId)
        loadBookMap(query, currentBookId, currentUserBookId);

        if (currentBookId == 0) {
          // Search Hardcover for this book
          auto err = HardcoverClient::searchBook(query, currentBookId);
          if (err != HardcoverClient::OK) {
            LOG_DBG(LOG_TAG, "Book search failed: %s", HardcoverClient::errorString(err));
            currentBookId = 0;
            currentUserBookId = 0;
            break;
          }
        }

        // Add to library as "Currently Reading" if not already tracked
        if (currentUserBookId == 0 && currentBookId != 0) {
          auto err =
              HardcoverClient::addBook(currentBookId, HardcoverClient::STATUS_CURRENTLY_READING, currentUserBookId);
          if (err != HardcoverClient::OK) {
            LOG_DBG(LOG_TAG, "Add book failed: %s", HardcoverClient::errorString(err));
            break;
          }
          // Cache the mapping
          saveBookMap(query, currentBookId, currentUserBookId);
        } else if (currentUserBookId != 0) {
          // Already tracked — just update status to reading
          HardcoverClient::updateStatus(currentUserBookId, HardcoverClient::STATUS_CURRENTLY_READING);
        }

        pageTurnCounter = 0;
        // lastSyncedPage and lastProgressPercent are preserved from loadSyncState() to resume correctly
        LOG_INF(LOG_TAG, "Tracking book_id=%d user_book=%d", currentBookId, currentUserBookId);
        break;
      }

      case SyncCmd::PAGE_TURN: {
        if (currentUserBookId == 0) break;

        // Without total page count from book metadata we cannot compute an
        // accurate progress_percentage.  Record the page for state persistence
        // but do NOT send a misleading value to Hardcover.
        // TODO: integrate with EPUB layout engine to get total page count
        //       for accurate percentage calculation.
        //
        // NOTE: Because lastProgressPercent remains PROGRESS_UNKNOWN here,
        // the FLUSH / updateProgress() path is effectively a no-op until a
        // reliable total page count is available to compute a real percentage.
        lastSyncedPage = msg.page;
        lastProgressPercent = PROGRESS_UNKNOWN;
        saveSyncState();
        break;
      }

      case SyncCmd::BOOK_CLOSE: {
        if (currentUserBookId == 0) break;

        // Do not infer "finished" from lastProgressPercent here.
        // The available progress value is not a reliable end-of-book signal.
        saveSyncState();
        currentBookId = 0;
        currentUserBookId = 0;
        pageTurnCounter = 0;
        break;
      }

      case SyncCmd::FLUSH: {
        if (currentUserBookId != 0 && lastProgressPercent != PROGRESS_UNKNOWN) {
          HardcoverClient::updateProgress(currentUserBookId, lastProgressPercent);
          saveSyncState();
        }
        // Signal the sender that the flush is complete (if a semaphore was attached)
        if (msg.completionSem) {
          xSemaphoreGive(msg.completionSem);
        }
        break;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Plugin hook implementations
// ---------------------------------------------------------------------------

void hardcoverBoot() {
  LOG_INF(LOG_TAG, "Hardcover plugin loaded");
  // Load credentials from SD so they are ready before the first sync
  HARDCOVER_STORE.loadFromFile();
}

void hardcoverEnable() {
  // Guard: skip creation if already running (e.g. dispatchBoot followed by
  // a redundant setEnabled call).
  if (syncQueue != nullptr) {
    LOG_DBG(LOG_TAG, "Sync task already running — skipping enable");
    return;
  }

  // Create the sync queue
  syncQueue = xQueueCreate(QUEUE_SIZE, sizeof(SyncMessage));
  if (!syncQueue) {
    LOG_ERR(LOG_TAG, "Failed to create sync queue");
    return;
  }

  // Create background sync task
  BaseType_t result = xTaskCreate(syncTaskFunc, "hc_sync", SYNC_TASK_STACK, nullptr, 1, &syncTaskHandle);
  if (result != pdPASS) {
    LOG_ERR(LOG_TAG, "Failed to create sync task");
    vQueueDelete(syncQueue);
    syncQueue = nullptr;
    return;
  }

  LOG_DBG(LOG_TAG, "Background sync task started");
}

void hardcoverDisable() {
  if (syncTaskHandle) {
    vTaskDelete(syncTaskHandle);
    syncTaskHandle = nullptr;
  }
  if (syncQueue) {
    // Drain any remaining messages before deleting the queue
    SyncMessage tmp;
    while (uxQueueMessagesWaiting(syncQueue) > 0) {
      xQueueReceive(syncQueue, &tmp, 0);
    }
    vQueueDelete(syncQueue);
    syncQueue = nullptr;
  }
  LOG_DBG(LOG_TAG, "Background sync task stopped");
}

void hardcoverBookOpen(const char* epubPath) {
  if (!syncQueue || !epubPath) return;

  SyncMessage msg = {};
  msg.cmd = SyncCmd::BOOK_OPEN;
  // Safe copy with truncation
  strncpy(msg.path, epubPath, sizeof(msg.path) - 1);
  msg.path[sizeof(msg.path) - 1] = '\0';

  xQueueSend(syncQueue, &msg, 0);  // Non-blocking (drop if full)
}

void hardcoverPageTurn(int chapter, int page) {
  if (!syncQueue) return;

  // Keep debounce state local to the hook so the hook does not race with the
  // background sync task over shared Hardcover state.
  static uint32_t localPageTurnCounter = 0;
  localPageTurnCounter++;

  // Debounce: only queue an update every N page turns.
  // Whether there is an active Hardcover book is background-task state; the
  // task should decide whether to apply or ignore this event.
  if (localPageTurnCounter % PAGE_TURN_SYNC_INTERVAL != 0) return;
  SyncMessage msg = {};
  msg.cmd = SyncCmd::PAGE_TURN;
  msg.chapter = chapter;
  msg.page = page;

  xQueueSend(syncQueue, &msg, 0);
}

void hardcoverBookClose() {
  if (!syncQueue) return;

  SyncMessage msg = {};
  msg.cmd = SyncCmd::BOOK_CLOSE;

  xQueueSend(syncQueue, &msg, 0);
}

void hardcoverSleep() {
  if (!syncQueue) return;

  SyncMessage msg = {};
  msg.cmd = SyncCmd::FLUSH;

  // Create a binary semaphore so we can block until the background task
  // finishes processing the FLUSH (i.e., updateProgress completes) before
  // the device enters deep sleep and loses the pending progress data.
  msg.completionSem = xSemaphoreCreateBinary();
  if (!msg.completionSem) {
    LOG_ERR(LOG_TAG, "Failed to create FLUSH semaphore — progress may be lost on sleep");
    xQueueSend(syncQueue, &msg, 0);
    return;
  }

  if (xQueueSend(syncQueue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
    LOG_ERR(LOG_TAG, "FLUSH queue send failed — progress may be lost on sleep");
    vSemaphoreDelete(msg.completionSem);
    return;
  }

  // Block until the sync task signals completion (FLUSH_TIMEOUT_MS timeout)
  if (xSemaphoreTake(msg.completionSem, pdMS_TO_TICKS(FLUSH_TIMEOUT_MS)) != pdTRUE) {
    LOG_ERR(LOG_TAG, "Timeout waiting for FLUSH completion — progress may be lost on sleep");
  }
  vSemaphoreDelete(msg.completionSem);
}

}  // namespace

// ---------------------------------------------------------------------------
// Plugin descriptor
// ---------------------------------------------------------------------------

extern const CprPlugin hardcoverPlugin = {
    .id = "hardcover_sync",
    .name = "Hardcover Sync",
    .version = "0.1.0",
    .author = "CrossPoint",
    .minCpr = CROSSPOINT_VERSION,
    .description = "Sync reading progress to Hardcover.app",

    .onBoot = hardcoverBoot,
    .onSettingsRender = nullptr,
    .onBookOpen = hardcoverBookOpen,
    .onBookClose = hardcoverBookClose,
    .onPageTurn = hardcoverPageTurn,
    .onSleep = hardcoverSleep,
    .onWake = nullptr,
    .onEnable = hardcoverEnable,
    .onDisable = hardcoverDisable,
};
