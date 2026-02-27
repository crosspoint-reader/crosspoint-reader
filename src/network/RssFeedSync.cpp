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

namespace {

constexpr const char* TAG = "FEED";
constexpr const char* SYNC_TIME_FILE = "/.crosspoint/feed-sync-time.bin";  // stores uint32_t epoch of last processed item
constexpr const char* LOG_FILE     = "/.crosspoint/feed-sync.log";
constexpr const char* NEWS_FILE = "/News.md";
constexpr size_t NEWS_MAX_SIZE = 50 * 1024;

TaskHandle_t syncTaskHandle = nullptr;
RssFeedSync::State s_state = RssFeedSync::State::IDLE;
int s_dlCurrent = 0;
int s_dlTotal   = 0;

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
  std::string crosspointType;  // "file", "image", "news", "firmware"
  std::string crosspointPath;  // target path on SD card
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
    if (errorOccurred) return length;

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
    if (parser && !errorOccurred) {
      if (XML_Parse(parser, nullptr, 0, XML_TRUE) != XML_STATUS_OK) {
        errorOccurred = true;
      }
    }
  }

  bool error() const { return errorOccurred; }
  const std::vector<RssItem>& getItems() const { return items; }

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

    if (!self->inItem) return;

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
      if (!self->currentItem.guid.empty()) {
        self->items.push_back(std::move(self->currentItem));
      }
      self->inItem = false;
      self->currentItem = RssItem{};
      self->activeField = Field::None;
      return;
    }

    if (!self->inItem) return;

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

  enum class Field { None, Guid, Title, Description, PubDate, CrosspointType, CrosspointPath };

  XML_Parser parser = nullptr;
  bool errorOccurred = false;
  bool inItem = false;
  Field activeField = Field::None;
  std::string currentText;
  RssItem currentItem;
  std::vector<RssItem> items;
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
// GUID dedup helpers
// ---------------------------------------------------------------------------
// Append a line to the feed sync log file
static void logToFile(const char* level, const char* msg) {
  FsFile file;
  Storage.mkdir("/.crosspoint");
  if (!Storage.openFileForWrite(TAG, LOG_FILE, file)) return;
  file.seekEnd(0);
  char buf[280];
  int n = snprintf(buf, sizeof(buf), "[%s] %s\n", level, msg);
  if (n > 0) file.write(reinterpret_cast<const uint8_t*>(buf), static_cast<size_t>(n));
  file.close();
}

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
  FsFile file;
  if (!Storage.openFileForRead(TAG, SYNC_TIME_FILE, file)) return 0;
  uint32_t t = 0;
  file.read(reinterpret_cast<uint8_t*>(&t), sizeof(t));
  file.close();
  return t;
}

static void saveLastSyncTime(uint32_t t) {
  Storage.mkdir("/.crosspoint");
  FsFile file;
  if (!Storage.openFileForWrite(TAG, SYNC_TIME_FILE, file)) return;
  file.write(reinterpret_cast<const uint8_t*>(&t), sizeof(t));
  file.close();
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
  logToFile("INFO", "Feed sync started");
  setState(RssFeedSync::State::FETCHING);

  const std::string feedUrl = SETTINGS.feedUrl;

  // 1. Fetch and parse the RSS feed
  RssParser rssParser;
  {
    RssParserStream stream(rssParser);
    if (!HttpDownloader::fetchUrl(feedUrl, stream)) {
      LOG_ERR(TAG, "Failed to fetch feed: %s", feedUrl.c_str());
      { char _b[320]; snprintf(_b, sizeof(_b), "ERROR: Failed to fetch feed URL (check server reachable, URL correct): %s", feedUrl.c_str()); logToFile("ERR", _b); }
      setState(RssFeedSync::State::ERROR);
      syncTaskHandle = nullptr;
      vTaskDelete(nullptr);
      return;
    }
  }  // stream destroyed here → parser.flush()

  setState(RssFeedSync::State::PARSING);

  if (rssParser.error()) {
    LOG_ERR(TAG, "Failed to parse feed XML");
    logToFile("ERR", "Failed to parse feed XML");
    setState(RssFeedSync::State::ERROR);
    syncTaskHandle = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  const auto& items = rssParser.getItems();
  LOG_DBG(TAG, "Parsed %u items from feed", items.size());
  { char _b[64]; snprintf(_b, sizeof(_b), "Parsed %u items from feed", (unsigned)items.size()); logToFile("INFO", _b); }

  if (items.empty()) {
    setState(RssFeedSync::State::DONE);
    syncTaskHandle = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  // 2. Load last sync timestamp — skip anything older than this
  const uint32_t lastSync = loadLastSyncTime();
  // We only advance the watermark after we've gone all the way back to lastSync.
  // oldestSuccess = timestamp of the oldest item we successfully processed this run.
  // We save oldestSuccess at the end — this ensures that any item that failed or
  // was interrupted between oldestSuccess and the newest item will be retried next sync.
  uint32_t oldestSuccess = 0;

  // Count new file/image items for progress display (feed is newest-first; stop at lastSync)
  s_dlCurrent = 0;
  s_dlTotal = 0;
  for (const auto& item : items) {
    const uint32_t t = parseRfc2822(item.pubDate);
    if (t > 0 && t <= lastSync) break;  // reached already-processed items
    if (item.crosspointType == "file" || item.crosspointType == "image") s_dlTotal++;
  }
  if (s_dlTotal > 0) setState(RssFeedSync::State::DOWNLOADING);

  // 3. Process each new item (stop when we reach items older than last sync)
  for (const auto& item : items) {
    const uint32_t itemTime = parseRfc2822(item.pubDate);
    if (itemTime > 0 && itemTime <= lastSync) break;  // done — rest already processed
    if (item.guid.empty()) continue;

    const auto& type = item.crosspointType;

    if (type == "file" || type == "image") {
      if (item.enclosureUrl.empty() || item.crosspointPath.empty()) {
        LOG_DBG(TAG, "Skipping %s item '%s': missing enclosure/path", type.c_str(), item.guid.c_str());
        continue;
      }
      ensureParentDir(item.crosspointPath);
      auto result = HttpDownloader::downloadToFile(item.enclosureUrl, item.crosspointPath);
      if (result != HttpDownloader::OK) {
        LOG_ERR(TAG, "Download failed for %s → %s", item.enclosureUrl.c_str(), item.crosspointPath.c_str());
        { char _b[256]; snprintf(_b, sizeof(_b), "Download failed: %s -> %s", item.enclosureUrl.c_str(), item.crosspointPath.c_str()); logToFile("ERR", _b); }
        continue;
      }
      s_dlCurrent++;
      LOG_DBG(TAG, "Downloaded %s → %s [%d/%d]", type.c_str(), item.crosspointPath.c_str(), s_dlCurrent, s_dlTotal);
      // Extract filename and add to shared received-files list for display
      const std::string& path = item.crosspointPath;
      const auto slash = path.rfind('/');
      UITheme::addReceivedFile(slash == std::string::npos ? path : path.substr(slash + 1));

    } else if (type == "firmware") {
      if (SETTINGS.feedAllowFirmware == 0) {
        LOG_ERR(TAG, "Skipping firmware item '%s': firmware updates disabled in settings", item.guid.c_str());
        continue;
      }
      if (item.enclosureUrl.empty()) {
        LOG_DBG(TAG, "Skipping firmware item '%s': missing enclosure", item.guid.c_str());
        continue;
      }
      auto result = HttpDownloader::downloadToFile(item.enclosureUrl, "/firmware.bin");
      if (result != HttpDownloader::OK) {
        LOG_ERR(TAG, "Firmware download failed: %s", item.enclosureUrl.c_str());
        { char _b[256]; snprintf(_b, sizeof(_b), "Firmware download failed: %s", item.enclosureUrl.c_str()); logToFile("ERR", _b); }
        continue;
      }
      LOG_DBG(TAG, "Firmware downloaded — will apply on next boot");

    } else if (type == "news") {
      prependNewsEntry(item);
      LOG_DBG(TAG, "Added news: %s", item.title.c_str());

    } else {
      LOG_DBG(TAG, "Unknown item type '%s' for guid '%s', skipping", type.c_str(), item.guid.c_str());
      continue;
    }

    // Record this item's timestamp — since feed is newest-first, each iteration
    // is older than the last, so this keeps getting overwritten with older values.
    // After the loop completes fully, oldestSuccess = oldest item we processed.
    const uint32_t t = parseRfc2822(item.pubDate);
    if (t > 0) oldestSuccess = t;
  }

  // Save watermark: only after the loop ran all the way back.
  // We save the timestamp of the OLDEST item we processed — anything newer that
  // failed will be retried next sync (they have newer timestamps > oldestSuccess).
  // We do NOT save if the loop never processed any items successfully.
  if (oldestSuccess > lastSync) saveLastSyncTime(oldestSuccess);

  LOG_DBG(TAG, "Feed sync complete");
  { char _b[64]; snprintf(_b, sizeof(_b), "Sync complete — %d files downloaded", s_dlCurrent); logToFile("INFO", _b); }
  setState(RssFeedSync::State::DONE);
  syncTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

}  // namespace

namespace RssFeedSync {

void startSync() {
  // Guard: feed URL must be configured
  if (strlen(SETTINGS.feedUrl) == 0) return;

  // Guard: must be connected to WiFi in STA mode
  if (WiFi.getMode() != WIFI_STA || WiFi.status() != WL_CONNECTED) return;

  // Guard: only one sync at a time
  if (syncTaskHandle != nullptr) return;

  s_state = RssFeedSync::State::FETCHING;  // set immediately so indicator lights before task starts
  s_dlCurrent = 0; s_dlTotal = 0;
  xTaskCreate(syncTask, "FeedSync", 8192, nullptr, 1, &syncTaskHandle);
}

State getState()    { return s_state; }
bool isFeedActive() { return s_state != RssFeedSync::State::IDLE && s_state != RssFeedSync::State::DONE && s_state != RssFeedSync::State::ERROR; }
bool isSyncing()    { return s_state == RssFeedSync::State::DOWNLOADING; }

const char* getStatusLabel() {
  switch (s_state) {
    case RssFeedSync::State::FETCHING:    return "FEED";
    case RssFeedSync::State::PARSING:     return "SYNC";
    case RssFeedSync::State::DOWNLOADING: {
      // "n/nn" progress — written into a static buffer
      static char buf[8];
      snprintf(buf, sizeof(buf), "%d/%d", s_dlCurrent + 1, s_dlTotal);
      return buf;
    }
    case RssFeedSync::State::ERROR:       return "ERR!";
    case RssFeedSync::State::DONE:        return "DONE";
    default:                 return "FEED";
  }
}

}  // namespace RssFeedSync
