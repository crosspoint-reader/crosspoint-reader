#include "RssFeedSync.h"
#include "components/UITheme.h"

#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>
#include <expat.h>

#include <cstring>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "HttpDownloader.h"

extern volatile bool dzFlashRequested;

namespace {

constexpr const char* TAG = "FEED";
constexpr const char* SYNC_TIME_FILE = "/.crosspoint/feed_sync_time.txt";  // decimal Unix epoch of last processed item; SD-persistent across reflashes
constexpr const char* FEED_MANIFEST_FILE = "/.crosspoint/feed_manifest.txt";  // full paths of files received in the last sync (one per line)
constexpr const char* NEWS_FILE = "/News.md";
constexpr size_t NEWS_MAX_SIZE = 50 * 1024;

TaskHandle_t syncTaskHandle = nullptr;
RssFeedSync::State s_state = RssFeedSync::State::IDLE;
int s_dlCurrent = 0;
int s_dlTotal   = 0;
unsigned long s_doneTime = 0;         // millis() when DONE was set, for auto-clear
unsigned long s_suppressStartMs = 0;  // millis() when suppressSync() was called (0 = not suppressed)
unsigned long s_suppressDurationMs = 0;

static void setState(RssFeedSync::State st) { s_state = st; }


// ---------------------------------------------------------------------------
// RSS item model
// ---------------------------------------------------------------------------
struct RssItem {
  std::string guid;
  std::string title;
  std::string description;
  std::string pubDate;
  std::string enclosureUrl;
  uint32_t enclosureLength = 0;   // from enclosure length="" attribute; 0 = unknown
  std::string crosspointType;     // "file", "image", "news", "firmware"
  std::string crosspointPath;     // target path on SD card
};

// ---------------------------------------------------------------------------
// SAX-style RSS parser (internal, modeled on OpdsParser)
// ---------------------------------------------------------------------------
class RssParser final : public Print {
 public:
  RssParser() {
    parser = XML_ParserCreateNS(nullptr, '|');
    if (!parser) {
      errorOccurred = true;
      LOG_ERR(TAG, "Failed to create XML parser");
    }
  }

  ~RssParser() override {
    if (parser) {
      XML_StopParser(parser, XML_FALSE);
      XML_SetElementHandler(parser, nullptr, nullptr);
      XML_SetCharacterDataHandler(parser, nullptr);
      XML_ParserFree(parser);
      parser = nullptr;
    }
  }

  RssParser(const RssParser&) = delete;
  RssParser& operator=(const RssParser&) = delete;

  size_t write(uint8_t c) override { return write(&c, 1); }

  size_t write(const uint8_t* data, size_t length) override {
    if (errorOccurred || stopped) return length;  // parser freed on stop — nothing to do

    XML_SetUserData(parser, this);
    XML_SetElementHandler(parser, onStartElement, onEndElement);
    XML_SetCharacterDataHandler(parser, onCharacterData);

    const char* pos = reinterpret_cast<const char*>(data);
    size_t remaining = length;
    constexpr size_t chunkSize = 1024;

    while (remaining > 0) {
      void* buf = XML_GetBuffer(parser, chunkSize);
      if (!buf) {
        errorOccurred = true;
        LOG_ERR(TAG, "XML buffer alloc failed");
        XML_ParserFree(parser);
        parser = nullptr;
        return length;
      }
      const size_t toRead = remaining < chunkSize ? remaining : chunkSize;
      memcpy(buf, pos, toRead);

      if (XML_ParseBuffer(parser, static_cast<int>(toRead), 0) == XML_STATUS_ERROR) {
        if (stopped) return length;  // intentional stop — not an error
        errorOccurred = true;
        LOG_ERR(TAG, "XML parse error line %lu: %s", XML_GetCurrentLineNumber(parser),
                XML_ErrorString(XML_GetErrorCode(parser)));
        XML_ParserFree(parser);
        parser = nullptr;
        return length;
      }
      pos += toRead;
      remaining -= toRead;
    }
    return length;
  }

  void flush() override {
    if (parser && !errorOccurred && !stopped) {
      if (XML_Parse(parser, nullptr, 0, XML_TRUE) != XML_STATUS_OK) {
        if (!stopped) errorOccurred = true;
      }
    }
  }

  bool error() const { return errorOccurred; }

  // Abort parsing early (e.g. once we've seen all new items — no need to read the rest).
  // Frees the Expat parser immediately to reclaim its internal state memory.
  void stopParsing() {
    if (parser) {
      XML_StopParser(parser, XML_FALSE);
      XML_SetElementHandler(parser, nullptr, nullptr);
      XML_SetCharacterDataHandler(parser, nullptr);
      XML_ParserFree(parser);
      parser = nullptr;
    }
    stopped = true;
  }

  using ItemCallback = std::function<void(const RssItem&)>;
  void setItemCallback(ItemCallback cb) { itemCallback = std::move(cb); }

  uint32_t getChannelItemCount() const { return channelItemCount; }

 private:
  // Check if the local part of a namespace-qualified name matches.
  // With XML_ParserCreateNS, names come as "nsuri|localname" or just "localname".
  static bool nameEquals(const char* fullName, const char* localName) {
    const char* sep = strrchr(fullName, '|');
    const char* local = sep ? sep + 1 : fullName;
    return strcmp(local, localName) == 0;
  }

  static bool nameEqualsNS(const char* fullName, const char* nsPrefix, const char* localName) {
    // For crosspoint:type → nsuri|type, check that the namespace contains the prefix
    const char* sep = strrchr(fullName, '|');
    if (!sep) return false;
    const char* local = sep + 1;
    if (strcmp(local, localName) != 0) return false;
    // Check that the namespace URI portion contains the prefix string
    return strstr(fullName, nsPrefix) != nullptr && strstr(fullName, nsPrefix) < sep;
  }

  static const char* findAttribute(const XML_Char** atts, const char* name) {
    for (int i = 0; atts[i]; i += 2) {
      // Check local name part after any namespace separator
      const char* sep = strrchr(atts[i], '|');
      const char* local = sep ? sep + 1 : atts[i];
      if (strcmp(local, name) == 0) return atts[i + 1];
    }
    return nullptr;
  }

  static void XMLCALL onStartElement(void* userData, const XML_Char* name, const XML_Char** atts) {
    auto* self = static_cast<RssParser*>(userData);

    if (nameEquals(name, "item")) {
      self->inItem = true;
      self->currentItem = RssItem{};
      return;
    }

    if (!self->inItem) {
      // Channel-level elements (before first item or between items)
      if (nameEqualsNS(name, "crosspoint", "itemCount")) {
        self->activeField = Field::ItemCount;
        self->currentText.clear();
      }
      return;
    }

    if (nameEquals(name, "guid")) {
      self->activeField = Field::Guid;
      self->currentText.clear();
    } else if (nameEquals(name, "title")) {
      self->activeField = Field::Title;
      self->currentText.clear();
    } else if (nameEquals(name, "description")) {
      self->activeField = Field::Description;
      self->currentText.clear();
    } else if (nameEquals(name, "pubDate")) {
      self->activeField = Field::PubDate;
      self->currentText.clear();
    } else if (nameEquals(name, "enclosure")) {
      const char* url = findAttribute(atts, "url");
      if (url) self->currentItem.enclosureUrl = url;
      const char* len = findAttribute(atts, "length");
      if (len) self->currentItem.enclosureLength = static_cast<uint32_t>(strtoul(len, nullptr, 10));
    } else if (nameEqualsNS(name, "crosspoint", "type")) {
      self->activeField = Field::CrosspointType;
      self->currentText.clear();
    } else if (nameEqualsNS(name, "crosspoint", "path")) {
      self->activeField = Field::CrosspointPath;
      self->currentText.clear();
    }
  }

  static void XMLCALL onEndElement(void* userData, const XML_Char* name) {
    auto* self = static_cast<RssParser*>(userData);

    if (nameEquals(name, "item")) {
      if (!self->currentItem.guid.empty() && self->itemCallback) {
        self->itemCallback(self->currentItem);
      }
      self->inItem = false;
      self->currentItem = RssItem{};
      self->activeField = Field::None;
      return;
    }

    if (!self->inItem) {
      // Channel-level element ending
      if (self->activeField == Field::ItemCount && !self->currentText.empty()) {
        self->channelItemCount = static_cast<uint32_t>(strtoul(self->currentText.c_str(), nullptr, 10));
      }
      self->activeField = Field::None;
      return;
    }

    switch (self->activeField) {
      case Field::Guid:
        self->currentItem.guid = self->currentText;
        break;
      case Field::Title:
        self->currentItem.title = self->currentText;
        break;
      case Field::Description:
        self->currentItem.description = self->currentText;
        break;
      case Field::PubDate:
        self->currentItem.pubDate = self->currentText;
        break;
      case Field::CrosspointType:
        self->currentItem.crosspointType = self->currentText;
        break;
      case Field::CrosspointPath:
        self->currentItem.crosspointPath = self->currentText;
        break;
      default:
        break;
    }
    self->activeField = Field::None;
  }

  static void XMLCALL onCharacterData(void* userData, const XML_Char* s, int len) {
    auto* self = static_cast<RssParser*>(userData);
    if (self->activeField != Field::None) {
      self->currentText.append(s, len);
    }
  }

  enum class Field { None, Guid, Title, Description, PubDate, CrosspointType, CrosspointPath, ItemCount };

  XML_Parser parser = nullptr;
  bool errorOccurred = false;
  bool stopped = false;
  bool inItem = false;
  Field activeField = Field::None;
  uint32_t channelItemCount = 0;
  std::string currentText;
  RssItem currentItem;
  ItemCallback itemCallback;
};

// ---------------------------------------------------------------------------
// Stream adapter for RssParser (same pattern as OpdsParserStream)
// ---------------------------------------------------------------------------
class RssParserStream : public Stream {
 public:
  explicit RssParserStream(RssParser& p) : parser(p) {}
  ~RssParserStream() override { parser.flush(); }

  int available() override { return 0; }
  int peek() override { return -1; }
  int read() override { return -1; }

  size_t write(uint8_t c) override { return parser.write(c); }
  size_t write(const uint8_t* buffer, size_t size) override { return parser.write(buffer, size); }

 private:
  RssParser& parser;
};

// ---------------------------------------------------------------------------
// Sync timestamp helpers  (stored as decimal Unix epoch in /feed_sync_time.txt)
// ---------------------------------------------------------------------------

// Parse RFC 2822 "Fri, 27 Feb 2026 18:00:11 +0000" → Unix epoch seconds. Returns 0 on failure.
static uint32_t parseRfc2822(const std::string& s) {
  static const char* months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                  "Jul","Aug","Sep","Oct","Nov","Dec"};
  int day=0, year=0, hour=0, min=0, sec=0, month=0;
  char mon[4] = {};
  if (sscanf(s.c_str(), "%*3s, %d %3s %d %d:%d:%d",
             &day, mon, &year, &hour, &min, &sec) < 6) return 0;
  for (int i = 0; i < 12; i++) {
    if (strncasecmp(mon, months[i], 3) == 0) { month = i + 1; break; }
  }
  if (month == 0 || year < 1970) return 0;
  int y = year - 1970;
  uint32_t days = (uint32_t)y * 365u + (uint32_t)((y + 1) / 4);
  static const int md[] = {0,31,59,90,120,151,181,212,243,273,304,334};
  days += (uint32_t)md[month - 1] + (uint32_t)(day - 1);
  if (month > 2 && (year % 4 == 0)) days++;
  return days * 86400u + (uint32_t)hour * 3600u + (uint32_t)min * 60u + (uint32_t)sec;
}

static uint32_t loadLastSyncTime() {
  char buf[16] = {};
  const size_t n = Storage.readFileToBuffer(SYNC_TIME_FILE, buf, sizeof(buf));
  if (n == 0) return 0;
  return static_cast<uint32_t>(strtoul(buf, nullptr, 10));
}

static void saveLastSyncTime(uint32_t t) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(t));
  Storage.writeFile(SYNC_TIME_FILE, String(buf));
}

// ---------------------------------------------------------------------------
// News.md helpers
// ---------------------------------------------------------------------------
void prependNewsEntry(const RssItem& item) {
  // Read existing content
  std::string existing;
  {
    FsFile file;
    if (Storage.openFileForRead(TAG, NEWS_FILE, file)) {
      size_t sz = file.size();
      if (sz > NEWS_MAX_SIZE) sz = NEWS_MAX_SIZE;
      existing.resize(sz);
      file.read(reinterpret_cast<uint8_t*>(existing.data()), sz);
      file.close();
    }
  }

  // Build new entry: ## Title\n*Date*\n\nBody.\n\n---\n\n
  std::string entry;
  entry.reserve(256);
  entry += "## ";
  entry += item.title.empty() ? "Untitled" : item.title;
  entry += "\n";
  if (!item.pubDate.empty()) {
    entry += "*";
    entry += item.pubDate;
    entry += "*\n";
  }
  entry += "\n";
  entry += item.description;
  entry += "\n\n---\n\n";

  // Write: new entry + existing
  std::string combined = entry + existing;

  FsFile file;
  if (Storage.openFileForWrite(TAG, NEWS_FILE, file)) {
    file.write(reinterpret_cast<const uint8_t*>(combined.c_str()), combined.size());
    file.close();
  }
}

// ---------------------------------------------------------------------------
// Ensure parent directory exists for a given file path
// ---------------------------------------------------------------------------
void ensureParentDir(const std::string& path) {
  size_t pos = path.rfind('/');
  if (pos != std::string::npos && pos > 0) {
    std::string dir = path.substr(0, pos);
    Storage.ensureDirectoryExists(dir.c_str());
  }
}

// ---------------------------------------------------------------------------
// Background sync task
// ---------------------------------------------------------------------------
void syncTask(void*) {
  LOG_DBG(TAG, "Feed sync started");
  LOG_INF(TAG, "Feed sync started");

  // Heap guard: abort early if memory is critically low rather than crashing
  // mid-parse from an OOM inside std::string / std::vector operations.
  constexpr uint32_t MIN_HEAP_FOR_SYNC = 30000;
  const uint32_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < MIN_HEAP_FOR_SYNC) {
    LOG_ERR(TAG, "Insufficient heap (%lu bytes) for feed sync — aborting", (unsigned long)freeHeap);
    setState(RssFeedSync::State::ERROR);
    syncTaskHandle = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  setState(RssFeedSync::State::FETCHING);

  // Open feed log file — overwrites each run so /api/feed/log shows the latest sync.
  HalFile logFile;
  Storage.openFileForWrite(TAG, "/.crosspoint/feed-sync.log", logFile);
  auto feedLog = [&](const char* msg) {
    if (!logFile.isOpen()) return;
    logFile.print(msg);
    logFile.print("\n");
    logFile.flush();
  };
  feedLog("--- sync start ---");

  // Open feed manifest — records the full path of every file downloaded this run.
  // Truncates the previous manifest so Browse → Feed only shows the latest sync.
  HalFile manifestFile;
  Storage.openFileForWrite(TAG, FEED_MANIFEST_FILE, manifestFile);

  // Wait for WiFi stack to be fully routed. WL_CONNECTED can fire before
  // DHCP/DNS/default-route are ready, causing immediate TCP failures.
  for (int attempt = 0; attempt < 10; attempt++) {
    if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) break;
    vTaskDelay(pdMS_TO_TICKS(300));
  }

  const std::string feedUrl = SETTINGS.feedUrl;
  LOG_INF(TAG, "Feed URL: %s", feedUrl.c_str());
  {
    char buf[160];
    snprintf(buf, sizeof(buf), "url: %s", feedUrl.c_str());
    feedLog(buf);
  }

  // 2. Load last sync timestamp — skip items older than this
  const uint32_t lastSync = loadLastSyncTime();
  LOG_INF(TAG, "Last sync timestamp: %lu", (unsigned long)lastSync);
  uint32_t oldestSuccess = 0;
  s_dlCurrent = 0;
  s_dlTotal = 0;

  LOG_INF(TAG, "Free heap before fetch: %lu bytes", (unsigned long)ESP.getFreeHeap());

  // 1+3. Fetch AND process items simultaneously via SAX callback.
  //      Items are processed as each </item> is parsed — no vector accumulation.
  //      The parser calls our callback inline during the fetch loop.
  //      Feed is newest-first; we stop once we hit items older than lastSync.
  bool reachedOldItems = false;
  RssParser rssParser;
  rssParser.setItemCallback([&](const RssItem& item) {
    if (reachedOldItems) return;

    // Pick up channel-level item count once it's parsed (channel header precedes items)
    if (s_dlTotal == 0 && rssParser.getChannelItemCount() > 0) {
      s_dlTotal = static_cast<int>(rssParser.getChannelItemCount());
    }

    const uint32_t itemTime = parseRfc2822(item.pubDate);
    if (itemTime > 0 && itemTime <= lastSync) {
      reachedOldItems = true;
      LOG_INF(TAG, "Reached already-processed items - stopping parse");
      rssParser.stopParsing();  // abort XML parse early — no need to read rest of feed
      return;
    }
    if (item.guid.empty()) return;

    const auto& type = item.crosspointType;
    LOG_INF(TAG, "Item: type=%s guid=%s", type.c_str(), item.guid.c_str());

    if (type == "file" || type == "image") {
      setState(RssFeedSync::State::DOWNLOADING);  // switch indicator as soon as first download starts
      if (item.enclosureUrl.empty() || item.crosspointPath.empty()) {
        LOG_DBG(TAG, "Skipping %s item '%s': missing enclosure/path", type.c_str(), item.guid.c_str());
        return;
      }
      // crosspointPath is a destination directory (ends with '/'); append filename from URL.
      std::string destPath = item.crosspointPath;
      if (!destPath.empty() && destPath.back() == '/') {
        const auto lastSlash = item.enclosureUrl.rfind('/');
        if (lastSlash != std::string::npos) {
          destPath += item.enclosureUrl.substr(lastSlash + 1);
        }
      }
      // Skip if already on SD card with matching size (guard against reflash / partial downloads).
      if (Storage.exists(destPath.c_str())) {
        HalFile existingFile = Storage.open(destPath.c_str());
        const size_t existingSize = existingFile.fileSize();
        existingFile.close();
        const bool sizeOk = (item.enclosureLength == 0) || (existingSize == static_cast<size_t>(item.enclosureLength));
        if (sizeOk) {
          LOG_DBG(TAG, "Skip (exists, size ok %u): %s", static_cast<unsigned>(existingSize), destPath.c_str());
          if (itemTime > 0) oldestSuccess = itemTime;
          return;
        }
        LOG_INF(TAG, "Re-download (size %u != expected %u): %s",
                static_cast<unsigned>(existingSize), static_cast<unsigned>(item.enclosureLength), destPath.c_str());
      }
      ensureParentDir(destPath);
      if (HttpDownloader::downloadToFile(item.enclosureUrl, destPath) != HttpDownloader::OK) {
        LOG_ERR(TAG, "Download failed: %s -> %s", item.enclosureUrl.c_str(), destPath.c_str());
        return;
      }
      s_dlCurrent++;
      LOG_INF(TAG, "Downloaded [%d]: %s (heap: %lu)", s_dlCurrent, destPath.c_str(), (unsigned long)ESP.getFreeHeap());
      const auto slash = destPath.rfind('/');
      UITheme::addReceivedFile(slash == std::string::npos ? destPath : destPath.substr(slash + 1));
      // Append to manifest so Browse → Feed virtual folder surfaces this file
      if (manifestFile.isOpen()) {
        manifestFile.print(destPath.c_str());
        manifestFile.print("\n");
        manifestFile.flush();
      }

    } else if (type == "firmware") {
      if (SETTINGS.feedAllowFirmware == 0) {
        LOG_ERR(TAG, "Skipping firmware item '%s': firmware updates disabled in settings", item.guid.c_str());
        return;
      }
      if (item.enclosureUrl.empty()) {
        LOG_DBG(TAG, "Skipping firmware item '%s': missing enclosure", item.guid.c_str());
        return;
      }
      // Skip if this firmware GUID references the currently running build (prevents flash loops).
      // Feed GUID is "firmware-{sha}" (e.g. "firmware-00430dd"); CROSSPOINT_GIT_SHA is the short
      // SHA baked in at compile time.  If the GUID contains our SHA, we're already running it.
      if (strstr(item.guid.c_str(), CROSSPOINT_GIT_SHA) != nullptr) {
        LOG_INF(TAG, "Skip firmware: GUID %s matches running SHA " CROSSPOINT_GIT_SHA, item.guid.c_str());
        if (itemTime > 0) oldestSuccess = itemTime;
        return;
      }
      // Skip if firmware.bin already on SD with matching size (e.g. after reboot before flash ran)
      if (Storage.exists("/firmware.bin") && item.enclosureLength > 0) {
        HalFile f = Storage.open("/firmware.bin");
        const size_t sz = f.fileSize();
        f.close();
        if (sz == static_cast<size_t>(item.enclosureLength)) {
          LOG_INF(TAG, "Skip firmware: /firmware.bin exists, size matches (%u)", static_cast<unsigned>(sz));
          if (itemTime > 0) oldestSuccess = itemTime;
          return;
        }
      }
      if (HttpDownloader::downloadToFile(item.enclosureUrl, "/firmware.bin") != HttpDownloader::OK) {
        LOG_ERR(TAG, "Firmware download failed: %s", item.enclosureUrl.c_str());
        Storage.remove("/firmware.bin");  // Delete any partial download — prevent flash loop
        return;
      }
      LOG_DBG(TAG, "Firmware downloaded — will apply on next boot");
      if (SETTINGS.dangerZoneEnabled) {
        // Persist the current watermark NOW before reboot — new firmware reads this file on
        // first boot so it doesn't re-download the entire feed (SD card survives OTA flash).
        if (itemTime > lastSync) saveLastSyncTime(itemTime);
        LOG_DBG(TAG, "Danger Zone enabled — auto-triggering flash");
        dzFlashRequested = true;
      }

    } else if (type == "news") {
      prependNewsEntry(item);
      LOG_DBG(TAG, "Added news: %s", item.title.c_str());

    } else {
      LOG_DBG(TAG, "Unknown item type '%s' for guid '%s', skipping", type.c_str(), item.guid.c_str());
      return;
    }

    // Track oldest successfully processed item (feed is newest-first, so this
    // keeps getting overwritten with progressively older timestamps).
    const uint32_t t = parseRfc2822(item.pubDate);
    if (t > 0) oldestSuccess = t;
  });

  // Now fetch — the callback fires for each item as it is parsed during the fetch.
  {
    RssParserStream stream(rssParser);
    LOG_INF(TAG, "Starting feed fetch...");
    setState(RssFeedSync::State::PARSING);
    if (!HttpDownloader::fetchUrl(feedUrl, stream)) {
      LOG_ERR(TAG, "FETCH FAILED url=%s heap=%lu", feedUrl.c_str(), (unsigned long)ESP.getFreeHeap());
      feedLog("FETCH FAILED");
      logFile.close();
      if (manifestFile.isOpen()) manifestFile.close();
      setState(RssFeedSync::State::ERROR);
      syncTaskHandle = nullptr;
      vTaskDelete(nullptr);
      return;
    }
  }  // stream destroyed here → parser.flush() → any final item callback fires
  LOG_INF(TAG, "Feed fetch complete");
  LOG_INF(TAG, "Free heap after fetch: %lu bytes", (unsigned long)ESP.getFreeHeap());

  if (rssParser.error()) {
    LOG_ERR(TAG, "XML parse error in feed");
    feedLog("XML PARSE ERROR");
    logFile.close();
    if (manifestFile.isOpen()) manifestFile.close();
    setState(RssFeedSync::State::ERROR);
    syncTaskHandle = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  // Save watermark after fetch+parse fully complete.
  // oldestSuccess = timestamp of oldest item we processed this run.
  if (oldestSuccess > lastSync) saveLastSyncTime(oldestSuccess);

  LOG_DBG(TAG, "Feed sync complete");
  LOG_INF(TAG, "Sync complete - %d files downloaded", s_dlCurrent);
  {
    char buf[48];
    snprintf(buf, sizeof(buf), "done: %d files", s_dlCurrent);
    feedLog(buf);
  }
  logFile.close();
  if (manifestFile.isOpen()) manifestFile.close();
  s_doneTime = millis();
  setState(RssFeedSync::State::DONE);
  syncTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

}  // namespace

namespace RssFeedSync {

void startSync() {
  // Guard: suppressed by user (button held at WiFi connect time)
  if (s_suppressStartMs != 0 && (millis() - s_suppressStartMs) < s_suppressDurationMs) {
    LOG_INF(TAG, "Feed sync suppressed by user request — skipping");
    return;
  }

  // Guard: feed URL must be configured
  if (strlen(SETTINGS.feedUrl) == 0) return;

  // Guard: must be connected to WiFi in STA mode
  if (WiFi.getMode() != WIFI_STA || WiFi.status() != WL_CONNECTED) return;

  // Guard: only one sync at a time
  if (syncTaskHandle != nullptr) return;

  s_state = RssFeedSync::State::FETCHING;  // set immediately so indicator lights before task starts
  s_dlCurrent = 0; s_dlTotal = 0;
  xTaskCreate(syncTask, "FeedSync", 16384, nullptr, 1, &syncTaskHandle);  // 16KB: HTTPS+Expat+std::string need headroom
}

void suppressSync(unsigned long durationMs) {
  s_suppressStartMs = millis();
  s_suppressDurationMs = durationMs;
  LOG_INF(TAG, "Feed sync suppressed for %lums", durationMs);
}

State getState() {
  if (s_state == State::DONE && millis() - s_doneTime > 5000) s_state = State::IDLE;
  return s_state;
}
bool isFeedActive() { return s_state != RssFeedSync::State::IDLE && s_state != RssFeedSync::State::DONE && s_state != RssFeedSync::State::ERROR; }
bool isSyncing()    { return s_state == RssFeedSync::State::DOWNLOADING; }
void getProgress(int& current, int& total) { current = s_dlCurrent; total = s_dlTotal; }

const char* getStatusLabel() {
  switch (s_state) {
    case RssFeedSync::State::FETCHING:    return "FEED";
    case RssFeedSync::State::PARSING:     return "SYNC";
    case RssFeedSync::State::DOWNLOADING: {
      // "n/nn" progress — written into a static buffer
      static char buf[8];
      if (s_dlTotal > 0) snprintf(buf, sizeof(buf), "%d/%d", s_dlCurrent + 1, s_dlTotal);
      else snprintf(buf, sizeof(buf), "#%d", s_dlCurrent + 1);
      return buf;
    }
    case RssFeedSync::State::ERROR:       return "ERR!";
    case RssFeedSync::State::DONE:        return "DONE";
    default:                 return "FEED";
  }
}

}  // namespace RssFeedSync
