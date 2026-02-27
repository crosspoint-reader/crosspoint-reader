#include "RssFeedSync.h"

#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>
#include <expat.h>

#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "HttpDownloader.h"

namespace {

constexpr const char* TAG = "FEED";
constexpr const char* SEEN_FILE = "/.crosspoint/feed-seen.txt";
constexpr const char* NEWS_FILE = "/News.md";
constexpr size_t NEWS_MAX_SIZE = 50 * 1024;

TaskHandle_t syncTaskHandle = nullptr;
bool s_feedActive = false;

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
std::set<std::string> loadSeenGuids() {
  std::set<std::string> seen;
  FsFile file;
  if (!Storage.openFileForRead(TAG, SEEN_FILE, file)) return seen;

  std::string line;
  while (file.available()) {
    char c;
    file.read(reinterpret_cast<uint8_t*>(&c), 1);
    if (c == '\n') {
      if (!line.empty()) seen.insert(std::move(line));
      line.clear();
    } else {
      line += c;
    }
  }
  if (!line.empty()) seen.insert(std::move(line));
  file.close();
  return seen;
}

void appendSeenGuid(const std::string& guid) {
  FsFile file;
  // Open in append mode by writing to the end
  if (!Storage.openFileForWrite(TAG, SEEN_FILE, file)) {
    // File doesn't exist yet — first write will create it
    Storage.mkdir("/.crosspoint");
    if (!Storage.openFileForWrite(TAG, SEEN_FILE, file)) return;
  }
  // Seek to end to append
  file.seekEnd(0);
  file.write(reinterpret_cast<const uint8_t*>(guid.c_str()), guid.size());
  file.write(reinterpret_cast<const uint8_t*>("\n"), 1);
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

  const std::string feedUrl = SETTINGS.feedUrl;

  // 1. Fetch and parse the RSS feed
  RssParser rssParser;
  {
    RssParserStream stream(rssParser);
    if (!HttpDownloader::fetchUrl(feedUrl, stream)) {
      LOG_ERR(TAG, "Failed to fetch feed: %s", feedUrl.c_str());
      syncTaskHandle = nullptr;
      s_feedActive = false;
      vTaskDelete(nullptr);
      return;
    }
  }  // stream destroyed here → parser.flush()

  if (rssParser.error()) {
    LOG_ERR(TAG, "Failed to parse feed XML");
    syncTaskHandle = nullptr;
    s_feedActive = false;
    vTaskDelete(nullptr);
    return;
  }

  const auto& items = rssParser.getItems();
  LOG_DBG(TAG, "Parsed %u items from feed", items.size());

  if (items.empty()) {
    syncTaskHandle = nullptr;
    s_feedActive = false;
    vTaskDelete(nullptr);
    return;
  }

  // 2. Load seen GUIDs
  auto seen = loadSeenGuids();

  // 3. Process each unseen item
  for (const auto& item : items) {
    if (item.guid.empty()) continue;
    if (seen.count(item.guid)) continue;

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
        continue;
      }
      LOG_DBG(TAG, "Downloaded %s → %s", type.c_str(), item.crosspointPath.c_str());

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

    appendSeenGuid(item.guid);
    seen.insert(item.guid);
  }

  LOG_DBG(TAG, "Feed sync complete");
  syncTaskHandle = nullptr;
  s_feedActive = false;
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

  s_feedActive = true;
  xTaskCreate(syncTask, "FeedSync", 8192, nullptr, 1, &syncTaskHandle);
}

bool isSyncing()    { return syncTaskHandle != nullptr; }
bool isFeedActive() { return s_feedActive; }

}  // namespace RssFeedSync
