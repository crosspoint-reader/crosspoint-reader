#include "ClippingSync.h"

#include <ArduinoJson.h>
#include <HalMemory.h>
#include <KOReaderCredentialStore.h>
#include <Logging.h>
#include <Memory.h>
#include <SecureHttpClient.h>
#include <Utf8.h>

#include <cstring>

#include "ClippingStore.h"

namespace {
// One item per response: 4 KiB text may occupy 24 KiB when JSON-escaped.
// Reuse one checked heap buffer for requests and responses; it cannot fit on the task stack.
constexpr size_t BUFFER_SIZE = 32 * 1024;

bool request(freeink::SecureHttpClient& http, const std::string& url, char* buffer, JsonDocument& doc,
             const bool upload = false) {
  size_t payloadSize = 0;
  if (upload) {
    if (doc.overflowed() || measureJson(doc) >= BUFFER_SIZE) {
      LOG_ERR("CLIP", "Clipping request exceeds buffer");
      return false;
    }
    payloadSize = serializeJson(doc, buffer, BUFFER_SIZE);
  }
  doc.clear();
  if (!http.begin(url)) {
    LOG_ERR("CLIP", "Invalid sync URL");
    return false;
  }
  http.addHeader("x-auth-user", KOREADER_STORE.getUsername());
  http.addHeader("x-auth-key", KOREADER_STORE.getMd5Password());
  http.addHeader("Content-Type", "application/json");
  size_t received = 0;
  const int status = http.sendRequest(upload ? "PUT" : "GET", reinterpret_cast<const uint8_t*>(buffer), payloadSize,
                                      [&](const uint8_t* data, const size_t length) {
                                        if (length >= BUFFER_SIZE - received) return false;
                                        memcpy(buffer + received, data, length);
                                        received += length;
                                        return true;
                                      });
  if (status != 200 || !http.responseComplete()) {
    LOG_ERR("CLIP", "Clipping sync HTTP %d (complete=%d)", status, http.responseComplete());
    return false;
  }
  buffer[received] = '\0';
  // Mutable input lets ArduinoJson reference the response buffer without duplicating text.
  const auto error = deserializeJson(doc, buffer, received, DeserializationOption::NestingLimit(4));
  if (error) {
    LOG_ERR("CLIP", "Invalid clipping response: %s", error.c_str());
    return false;
  }
  if (upload && doc["accepted"].as<int>() != 1) {
    LOG_ERR("CLIP", "Server did not acknowledge clipping");
    return false;
  }
  return true;
}

void encode(JsonObject item, const Clipping& clipping, const std::string& text) {
  item["id"] = clipping.id;
  item["spine"] = clipping.spineIndex;
  item["start_page"] = clipping.startPage;
  item["end_page"] = clipping.endPage;
  item["pages"] = clipping.pageCount;
  item["start_word"] = clipping.startWordIndex;
  item["end_word"] = clipping.endWordIndex;
  item["words"] = clipping.wordCount;
  if (clipping.paragraphIndex != UINT16_MAX) item["para"] = clipping.paragraphIndex;
  item["chapter"] = clipping.chapterTitle;
  item["text"] = text.c_str();
  item["created_at"] = clipping.timestamp;
  item["layout_signature"] = clipping.layoutSignature;
  if (clipping.startOffset != UINT32_MAX && clipping.endOffset != UINT32_MAX) {
    item["start_offset"] = clipping.startOffset;
    item["end_offset"] = clipping.endOffset;
  }
}

bool decode(JsonObjectConst item, Clipping& clipping, std::string& text, bool& deleted) {
  const JsonString id = item["id"].as<JsonString>();
  if (id.isNull() || id.size() == 0 || id.size() >= sizeof(clipping.id)) return false;
  for (size_t i = 0; i < id.size(); ++i) {
    if (id.c_str()[i] < '!' || id.c_str()[i] > '~') return false;
  }
  memcpy(clipping.id, id.c_str(), id.size());
  clipping.id[id.size()] = '\0';
  if (!item["deleted"].is<unsigned>() || item["deleted"].as<unsigned>() > 1) return false;
  deleted = item["deleted"].as<unsigned>() == 1;
  if (deleted) return true;
  if (!item["revision"].is<uint64_t>() || !item["spine"].is<uint16_t>() || !item["start_page"].is<uint16_t>() ||
      !item["end_page"].is<uint16_t>() || !item["pages"].is<uint16_t>() || !item["start_word"].is<uint16_t>() ||
      !item["end_word"].is<uint16_t>() || !item["words"].is<uint16_t>() || !item["created_at"].is<uint32_t>() ||
      !item["layout_signature"].is<uint32_t>())
    return false;
  if (!item["para"].isNull() && !item["para"].is<uint16_t>()) return false;
  const JsonString quote = item["text"].as<JsonString>();
  if (quote.isNull() || quote.size() == 0 || quote.size() > CLIPPING_TEXT_MAX || strlen(quote.c_str()) != quote.size())
    return false;
  clipping.spineIndex = item["spine"];
  clipping.startPage = item["start_page"];
  clipping.endPage = item["end_page"];
  clipping.pageCount = item["pages"];
  clipping.startWordIndex = item["start_word"];
  clipping.endWordIndex = item["end_word"];
  clipping.wordCount = item["words"];
  clipping.paragraphIndex = item["para"] | UINT16_MAX;
  clipping.timestamp = item["created_at"];
  clipping.layoutSignature = item["layout_signature"];
  clipping.syncRevision = item["revision"];
  clipping.pendingUpload = false;
  if (!item["start_offset"].isNull() || !item["end_offset"].isNull()) {
    if (!item["start_offset"].is<uint32_t>() || !item["end_offset"].is<uint32_t>()) return false;
    clipping.startOffset = item["start_offset"];
    clipping.endOffset = item["end_offset"];
    if (clipping.startOffset >= clipping.endOffset || clipping.endOffset == UINT32_MAX) return false;
  }
  snprintf(clipping.chapterTitle, sizeof(clipping.chapterTitle), "%s", item["chapter"] | "");
  clipping.chapterTitle[utf8SafeTruncateBuffer(clipping.chapterTitle, strlen(clipping.chapterTitle))] = '\0';
  text.assign(quote.c_str(), quote.size());
  clipping.textLength = static_cast<uint16_t>(text.size());
  return true;
}
}  // namespace

bool clippingSync::run(const std::string& bookPath, const std::string& documentHash) {
  // Gate before loading files, allocating buffers, or contacting the server.
  if (!KOREADER_STORE.getSyncClippings()) return true;
  if (!KOREADER_STORE.hasCredentials()) return false;
  if (!CLIPPINGS.loadForBook(bookPath, "", "", "epub")) return false;
  struct Unload {
    ~Unload() { CLIPPINGS.unload(); }
  } unload;

  auto buffer = makeUniqueNoThrow<char[]>(BUFFER_SIZE);
  auto http = makeUniqueNoThrow<freeink::SecureHttpClient>();
  const auto heap = HalMemory::getDefaultHeap();
  if (!buffer || !http || heap.freeBytes < 40000 || heap.largestBlockBytes < 20000) {
    LOG_ERR("CLIP", "Insufficient heap for clipping sync");
    return false;
  }
  http->setInsecure();
  const std::string url = KOREADER_STORE.getBaseUrl() + "/api/v1/clippings/" + documentHash;
  JsonDocument doc;
  // Probe the extension before any upload, even when the user explicitly enabled it.
  if (!request(*http, url + "?cursor=0&limit=1&format=reader", buffer.get(), doc) ||
      doc["sync_version"].as<unsigned>() != 2) {
    LOG_ERR("CLIP", "Clipping sync requires a compatible crosspoint-sync server");
    return false;
  }
  if (!CLIPPINGS.prepareSync()) return false;

  uint32_t deletionOffset = 0;
  char deletedId[65];
  while (true) {
    if (!CLIPPINGS.nextDeletion(deletionOffset, deletedId)) return false;
    if (!deletedId[0]) break;
    doc.clear();
    JsonObject item = doc["items"].to<JsonArray>().add<JsonObject>();
    item["id"] = deletedId;
    item["deleted"] = 1;
    if (!request(*http, url, buffer.get(), doc, true)) return false;
  }
  if (!CLIPPINGS.finishDeletions()) return false;

  std::string text;
  for (size_t i = 0; i < CLIPPINGS.clippingCount(); ++i) {
    const Clipping& clipping = *CLIPPINGS.clippingAt(i);
    if (!clipping.pendingUpload) continue;
    if (!CLIPPINGS.readClippingText(i, text)) return false;
    doc.clear();
    encode(doc["items"].to<JsonArray>().add<JsonObject>(), clipping, text);
    if (!request(*http, url, buffer.get(), doc, true)) return false;
  }
  if (!CLIPPINGS.finishUploads()) return false;

  // ponytail: rescan one book per manual sync; persist an account-scoped cursor if histories grow large.
  uint64_t cursor = 0;
  do {
    if (!request(*http, url + "?limit=1&format=reader&cursor=" + std::to_string(cursor), buffer.get(), doc))
      return false;
    if (!doc["cursor"].is<uint64_t>() || !doc["more"].is<bool>() || !doc["items"].is<JsonArray>()) return false;
    const uint64_t next = doc["cursor"];
    const JsonArrayConst items = doc["items"].as<JsonArrayConst>();
    if (items.size() > 1 || next < cursor || ((!items.isNull() && items.size() > 0) && next <= cursor) ||
        (doc["more"].as<bool>() && items.size() == 0)) {
      LOG_ERR("CLIP", "Invalid clipping pagination");
      return false;
    }
    for (const JsonObjectConst item : items) {
      Clipping clipping;
      bool deleted = false;
      if (!decode(item, clipping, text, deleted)) {
        LOG_ERR("CLIP", "Invalid remote clipping");
        return false;
      }
      if (!CLIPPINGS.applyRemote(clipping, text, deleted)) return false;
    }
    cursor = next;
  } while (doc["more"].as<bool>());
  return true;
}
