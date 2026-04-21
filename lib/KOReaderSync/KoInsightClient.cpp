#include "KoInsightClient.h"

#include <ArduinoJson.h>
#include <Epub.h>
#include <HTTPClient.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <unordered_set>

#include "../../src/BookStats.h"
#include "../../src/KoInsightPendingSessions.h"
#include "../../src/ReadingStats.h"
#include "../../src/RecentBooksStore.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderDocumentId.h"

namespace {
constexpr int64_t MIN_VALID_UNIX = 1704067200;  // 2024-01-01 UTC (same as ReadingStats NTP gate)

constexpr char KOINSIGHT_API_VERSION[] = "0.2.0";
constexpr char KOINSIGHT_DEVICE_MODEL[] = "CrossPoint";
constexpr char API_DEVICE[] = "/api/plugin/device";
constexpr char API_IMPORT[] = "/api/plugin/import";

bool isHttpsUrl(const std::string& url) { return url.rfind("https://", 0) == 0; }

std::string deviceIdString() {
  String mac = WiFi.macAddress();
  std::string id = "crosspoint-";
  for (char c : std::string(mac.c_str())) {
    if (c != ':') {
      id += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
  }
  return id;
}

std::string epubCacheKeyPath(const std::string& epubPath) {
  return std::string("/.crosspoint/epub_") + std::to_string(std::hash<std::string>{}(epubPath));
}

struct ProgressSnap {
  int spine = 0;
  int page = 0;
  int chapterPages = 0;
  bool ok = false;
};

ProgressSnap readProgressBin(const std::string& epubPath) {
  ProgressSnap p;
  FsFile f;
  const std::string path = epubCacheKeyPath(epubPath) + "/progress.bin";
  if (!Storage.openFileForRead("KOI", path, f)) {
    return p;
  }
  uint8_t d[6];
  if (f.read(d, 6) != 6) {
    return p;
  }
  p.spine = d[0] | (d[1] << 8);
  p.page = d[2] | (d[3] << 8);
  if (p.page == UINT16_MAX) {
    p.page = 0;
  }
  p.chapterPages = d[4] | (d[5] << 8);
  p.ok = true;
  return p;
}

std::string hashForBookPath(const std::string& path) {
  if (KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME) {
    return KOReaderDocumentId::calculateFromFilename(path);
  }
  return KOReaderDocumentId::calculate(path);
}

bool httpPostJson(const std::string& url, const std::string& body, const char* logTag) {
  HTTPClient http;
  std::unique_ptr<WiFiClientSecure> secureClient;
  WiFiClient plainClient;

  if (isHttpsUrl(url)) {
    secureClient = std::make_unique<WiFiClientSecure>();
    secureClient->setInsecure();
    http.begin(*secureClient, url.c_str());
  } else {
    http.begin(plainClient, url.c_str());
  }
  http.addHeader("Content-Type", "application/json");
  // ESP32 HTTPClient::POST(uint8_t*, size_t) is non-const; use String overload like other project HTTP calls.
  const int code = http.POST(String(body.c_str()));
  http.end();
  LOG_DBG(logTag, "POST %s -> %d", url.c_str(), code);
  if (code < 0) {
    LOG_ERR(logTag, "network error posting KoInsight");
    return false;
  }
  if (code != 200) {
    LOG_ERR(logTag, "KoInsight HTTP %d", code);
    return false;
  }
  return true;
}

void addBookJson(JsonArray& books, int& bookId, const std::string& md5, const std::string& title,
                 const std::string& authors, const std::string& language, int pagesField, uint32_t lastOpenUnix,
                 uint32_t totalReadSecs, int totalReadPages) {
  bookId++;
  JsonObject b = books.add<JsonObject>();
  b["id"] = bookId;
  b["title"] = title;
  b["authors"] = authors;
  b["notes"] = 0;
  b["last_open"] = static_cast<int64_t>(lastOpenUnix);
  b["highlights"] = 0;
  b["pages"] = pagesField;
  b["series"] = "";
  b["language"] = language;
  b["md5"] = md5;
  b["total_read_time"] = totalReadSecs;
  b["total_read_pages"] = totalReadPages;
}

std::string recentAuthorForPath(const std::string& path) {
  for (const RecentBook& rb : RECENT_BOOKS.getBooks()) {
    if (rb.path == path) return rb.author;
  }
  return "";
}

std::string recentTitleForPath(const std::string& path) {
  for (const RecentBook& rb : RECENT_BOOKS.getBooks()) {
    if (rb.path == path) return rb.title;
  }
  return "";
}

std::string buildImportPayload(const std::shared_ptr<Epub>& epub, const std::string& openEpubPath,
                               int /*openSpineIndex*/, int openPage, int openChapterTotalPages) {
  const std::string devId = deviceIdString();
  JsonDocument doc;

  JsonArray books = doc["books"].to<JsonArray>();
  JsonArray stats = doc["stats"].to<JsonArray>();
  doc["annotations"].to<JsonObject>();
  doc["version"] = KOINSIGHT_API_VERSION;

  std::unordered_set<std::string> bookMd5s;
  int bookId = 0;

  for (const auto& kv : BOOK_STATS.getBooks()) {
    const std::string& path = kv.first;
    if (!Storage.exists(path.c_str())) continue;
    const auto& e = kv.second;
    // Ignore bogus rows (e.g. old bug: sessions incremented with 0s duration when clock was unset).
    if (e.totalSeconds == 0) continue;

    const std::string md5 = hashForBookPath(path);
    if (md5.empty() || bookMd5s.count(md5)) continue;
    bookMd5s.insert(md5);

    const bool isOpen = static_cast<bool>(epub) && (path == openEpubPath);
    ProgressSnap snap = readProgressBin(path);
    int chPages = snap.chapterPages;
    if (isOpen && openChapterTotalPages > 0) {
      chPages = openChapterTotalPages;
    }
    const int pagesField = (chPages > 0) ? chPages : 1;

    std::string title = e.title[0] != '\0' ? std::string(e.title) : recentTitleForPath(path);
    std::string authors = recentAuthorForPath(path);
    std::string language;
    if (isOpen) {
      title = epub->getTitle();
      authors = epub->getAuthor();
      language = epub->getLanguage();
    }

    const int estPagesRead = std::max(0, static_cast<int>(e.progress) * pagesField / 100);

    addBookJson(books, bookId, md5, title, authors, language, pagesField, e.lastReadTimestamp, e.totalSeconds,
                estPagesRead);
  }

  const uint32_t liveSecs = READ_STATS.currentSessionElapsedSeconds();
  if (epub && !openEpubPath.empty() && Storage.exists(openEpubPath.c_str()) && liveSecs > 0) {
    const std::string md5Open = hashForBookPath(openEpubPath);
    if (!md5Open.empty() && !bookMd5s.count(md5Open)) {
      bookMd5s.insert(md5Open);
      const int chPages =
          (openChapterTotalPages > 0) ? openChapterTotalPages : readProgressBin(openEpubPath).chapterPages;
      const int pagesField = (chPages > 0) ? chPages : 1;
      addBookJson(books, bookId, md5Open, epub->getTitle(), epub->getAuthor(), epub->getLanguage(), pagesField,
                  static_cast<uint32_t>(time(nullptr)), liveSecs, 0);
    }
  }

  for (const auto& ps : KoInsightPendingSessions::loadAll()) {
    if (!Storage.exists(ps.path.c_str())) continue;
    JsonObject s = stats.add<JsonObject>();
    s["page"] = ps.page;
    s["start_time"] = ps.startTime;
    s["duration"] = ps.durationSec;
    s["total_pages"] = ps.totalPages > 0 ? ps.totalPages : 1;
    s["book_md5"] = ps.md5;
    s["device_id"] = devId;
  }

  if (READ_STATS.isSessionActive() && epub && !openEpubPath.empty() && liveSecs > 0) {
    const std::string md5Open = hashForBookPath(openEpubPath);
    if (!md5Open.empty()) {
      int64_t liveStart = static_cast<int64_t>(READ_STATS.sessionStartWallTime());
      if (liveStart < MIN_VALID_UNIX) {
        liveStart = static_cast<int64_t>(time(nullptr)) - static_cast<int64_t>(liveSecs);
      }
      JsonObject s = stats.add<JsonObject>();
      s["page"] = openPage + 1;
      s["start_time"] = liveStart;
      s["duration"] = liveSecs;
      s["total_pages"] = (openChapterTotalPages > 0) ? openChapterTotalPages : 1;
      s["book_md5"] = md5Open;
      s["device_id"] = devId;
    }
  }

  std::string out;
  serializeJson(doc, out);
  LOG_DBG("KoInsight", "import payload: books=%u stats=%u bytes=%u", static_cast<unsigned>(books.size()),
          static_cast<unsigned>(stats.size()), static_cast<unsigned>(out.size()));
  return out;
}
}  // namespace

void KoInsightClient::pushReadingSnapshotIfConfigured(const std::shared_ptr<Epub>& epub, const std::string& epubPath,
                                                      int currentSpineIndex, int currentPage, int chapterTotalPages) {
  if (!KOREADER_STORE.hasKoInsightServerUrl()) {
    return;
  }
  const std::string base = KOREADER_STORE.getKoInsightBaseUrl();
  if (base.empty()) {
    return;
  }

  const std::string devId = deviceIdString();

  JsonDocument devDoc;
  devDoc["id"] = devId;
  devDoc["model"] = KOINSIGHT_DEVICE_MODEL;
  devDoc["version"] = KOINSIGHT_API_VERSION;
  std::string devBody;
  serializeJson(devDoc, devBody);

  const std::string deviceUrl = base + API_DEVICE;
  if (!httpPostJson(deviceUrl, devBody, "KoInsight")) {
    LOG_ERR("KoInsight", "device POST failed; continuing with import (same as plugin)");
  }

  const std::string importBody = buildImportPayload(epub, epubPath, currentSpineIndex, currentPage, chapterTotalPages);
  const std::string importUrl = base + API_IMPORT;
  if (httpPostJson(importUrl, importBody, "KoInsight")) {
    KoInsightPendingSessions::clear();
  }
}
