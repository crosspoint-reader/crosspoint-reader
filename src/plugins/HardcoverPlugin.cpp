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

// FreeRTOS task stack size (bytes) — 4096 for network I/O
static constexpr uint32_t SYNC_TASK_STACK = 4096;

// Queue capacity (number of pending commands)
static constexpr int QUEUE_SIZE = 8;

// Paths on SD card
constexpr char STATE_DIR[] = "/.crosspoint/plugin_hardcover";
constexpr char STATE_FILE[] = "/.crosspoint/plugin_hardcover/state.json";
constexpr char BOOK_MAP_FILE[] = "/.crosspoint/plugin_hardcover/book_map.json";

// ---------------------------------------------------------------------------
// Sync command types queued from hooks → background task
// ---------------------------------------------------------------------------

enum class SyncCmd : uint8_t {
  BOOK_OPEN,    // Look up book, set "Currently Reading"
  PAGE_TURN,    // Update progress percentage
  BOOK_CLOSE,   // Flush final progress
  FLUSH,        // Flush pending (from onSleep)
};

struct SyncMessage {
  SyncCmd cmd;
  char path[128];   // EPUB path (BOOK_OPEN only)
  int chapter;      // Current chapter
  int page;         // Current page
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
float lastProgressPercent = 0.0f;

// ---------------------------------------------------------------------------
// SD persistence helpers
// ---------------------------------------------------------------------------

/// Load the cached book mapping (ISBN → book_id, user_book_id) from SD.
void loadBookMap(const char* isbn, int& bookId, int& userBookId) {
  bookId = 0;
  userBookId = 0;

  if (!Storage.exists(BOOK_MAP_FILE)) return;

  String json = Storage.readFile(BOOK_MAP_FILE);
  if (json.isEmpty()) return;

  JsonDocument doc;
  if (deserializeJson(doc, json)) return;

  JsonObject entry = doc[isbn];
  if (entry) {
    bookId = entry["book_id"] | 0;
    userBookId = entry["user_book_id"] | 0;
    LOG_DBG(LOG_TAG, "Cache hit: %s → book=%d ub=%d", isbn, bookId, userBookId);
  }
}

/// Save a book mapping entry to the SD cache.
void saveBookMap(const char* isbn, int bookId, int userBookId) {
  Storage.mkdir(STATE_DIR);

  // Load existing map (if any) and merge
  JsonDocument doc;
  if (Storage.exists(BOOK_MAP_FILE)) {
    String json = Storage.readFile(BOOK_MAP_FILE);
    if (!json.isEmpty()) {
      deserializeJson(doc, json);  // ignore errors — we'll overwrite anyway
    }
  }

  JsonObject entry = doc[isbn].to<JsonObject>();
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
      continue;
    }

    switch (msg.cmd) {
      case SyncCmd::BOOK_OPEN: {
        // Extract search query from EPUB path
        char query[128];
        extractSearchQuery(msg.path, query, sizeof(query));
        LOG_DBG(LOG_TAG, "Book opened, searching: %s", query);

        // Check cached mapping first
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
        lastSyncedPage = 0;
        lastProgressPercent = 0.0f;
        LOG_INF(LOG_TAG, "Tracking book_id=%d user_book=%d", currentBookId, currentUserBookId);
        break;
      }

      case SyncCmd::PAGE_TURN: {
        if (currentUserBookId == 0) break;

        // The page value from onPageTurn is a sequential page number.
        // Without total page count from book metadata we cannot compute
        // an accurate percentage.  Send the raw page number as a
        // progress_percentage clamped to [0,100].
        // TODO: integrate with EPUB layout engine to get total page count
        //       for accurate percentage calculation.
        lastProgressPercent = (msg.page > 0) ? static_cast<float>(msg.page) / 100.0f : 0.0f;
        if (lastProgressPercent > 1.0f) lastProgressPercent = 1.0f;

        auto err = HardcoverClient::updateProgress(currentUserBookId, lastProgressPercent);
        if (err == HardcoverClient::OK) {
          lastSyncedPage = msg.page;
          saveSyncState();
        } else {
          LOG_DBG(LOG_TAG, "Progress update failed: %s", HardcoverClient::errorString(err));
        }
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
        if (currentUserBookId == 0) break;
        if (lastSyncedPage > 0) {
          HardcoverClient::updateProgress(currentUserBookId, lastProgressPercent);
          saveSyncState();
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

  // Load credentials from SD
  HARDCOVER_STORE.loadFromFile();

  // Create the sync queue
  syncQueue = xQueueCreate(QUEUE_SIZE, sizeof(SyncMessage));
  if (!syncQueue) {
    LOG_ERR(LOG_TAG, "Failed to create sync queue");
    return;
  }

  // Create background sync task (4096 byte stack for network I/O)
  BaseType_t result =
      xTaskCreate(syncTaskFunc, "hc_sync", SYNC_TASK_STACK, nullptr, 1, &syncTaskHandle);
  if (result != pdPASS) {
    LOG_ERR(LOG_TAG, "Failed to create sync task");
    vQueueDelete(syncQueue);
    syncQueue = nullptr;
    return;
  }

  LOG_DBG(LOG_TAG, "Background sync task started");
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
  pageTurnCounter++;

  // Debounce: only queue an update every N page turns
  if (pageTurnCounter % PAGE_TURN_SYNC_INTERVAL != 0) return;
  if (!syncQueue || currentUserBookId == 0) return;

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

  xQueueSend(syncQueue, &msg, 0);
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
};
