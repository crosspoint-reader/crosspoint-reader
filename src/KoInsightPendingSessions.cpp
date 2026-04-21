#include "KoInsightPendingSessions.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include "KOReaderCredentialStore.h"
#include "KOReaderDocumentId.h"

namespace {
constexpr char PENDING_FILE[] = "/.crosspoint/koinsight_pending_sessions.json";
constexpr int MAX_ITEMS = 400;

std::string hashPath(const char* path) {
  if (!path || !path[0]) return "";
  const std::string p(path);
  if (KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME) {
    return KOReaderDocumentId::calculateFromFilename(p);
  }
  return KOReaderDocumentId::calculate(p);
}

void saveItems(const std::vector<KoInsightPendingStat>& items) {
  Storage.mkdir("/.crosspoint");
  JsonDocument doc;
  JsonArray arr = doc["items"].to<JsonArray>();
  for (const auto& it : items) {
    JsonObject o = arr.add<JsonObject>();
    o["path"] = it.path;
    o["md5"] = it.md5;
    o["st"] = it.startTime;
    o["dur"] = it.durationSec;
    o["pg"] = it.page;
    o["tp"] = it.totalPages;
  }
  String json;
  serializeJson(doc, json);
  if (!Storage.writeFile(PENDING_FILE, json)) {
    LOG_ERR("KOIQ", "Failed to save pending sessions");
  }
}
}  // namespace

void KoInsightPendingSessions::append(const char* bookPath, const int64_t sessionStartUnix, const uint32_t durationSec,
                                      const uint16_t pageOneBased, const uint16_t chapterTotalPages) {
  if (!bookPath || !bookPath[0] || durationSec == 0) return;

  const std::string md5 = hashPath(bookPath);
  if (md5.empty()) {
    LOG_ERR("KOIQ", "append: no md5 for path");
    return;
  }

  std::vector<KoInsightPendingStat> items = loadAll();
  KoInsightPendingStat row;
  row.path = bookPath;
  row.md5 = md5;
  row.startTime = sessionStartUnix;
  row.durationSec = durationSec;
  row.page = pageOneBased > 0 ? static_cast<int>(pageOneBased) : 1;
  row.totalPages = chapterTotalPages > 0 ? static_cast<int>(chapterTotalPages) : 1;
  items.push_back(std::move(row));

  while (static_cast<int>(items.size()) > MAX_ITEMS) {
    items.erase(items.begin());
  }
  saveItems(items);
}

std::vector<KoInsightPendingStat> KoInsightPendingSessions::loadAll() {
  std::vector<KoInsightPendingStat> out;
  if (!Storage.exists(PENDING_FILE)) {
    return out;
  }
  const String json = Storage.readFile(PENDING_FILE);
  if (json.isEmpty()) return out;

  JsonDocument doc;
  if (deserializeJson(doc, json)) {
    return out;
  }
  JsonArray arr = doc["items"].as<JsonArray>();
  for (JsonObject o : arr) {
    KoInsightPendingStat row;
    row.path = o["path"] | "";
    row.md5 = o["md5"] | "";
    row.startTime = o["st"] | (int64_t)0;
    row.durationSec = o["dur"] | (uint32_t)0;
    row.page = o["pg"] | 1;
    row.totalPages = o["tp"] | 1;
    if (!row.path.empty() && !row.md5.empty() && row.durationSec > 0) {
      out.push_back(std::move(row));
    }
  }
  return out;
}

void KoInsightPendingSessions::clear() { Storage.remove(PENDING_FILE); }
