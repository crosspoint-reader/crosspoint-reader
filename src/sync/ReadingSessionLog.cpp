#include "ReadingSessionLog.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <KOReaderCredentialStore.h>
#include <KOReaderSyncClient.h>
#include <Logging.h>
#include <PersistableStore.h>
#include <TrustedTime.h>
#include <esp_mac.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "SyncRequest.h"

namespace {
constexpr char LOG_PATH[] = "/.crosspoint/reading_sessions.jsonl";
// Upload position in the log, kept per account so a new account receives the whole history.
constexpr char STATE_PATH[] = "/.crosspoint/reading_sessions_sync.json";

constexpr size_t BATCH_SIZE = 20;  // server limit per request
// Bounds one manual sync to 200 sessions when a long history is first uploaded.
constexpr size_t MAX_BATCHES = 10;
constexpr size_t MAX_LINE = 160;

// Server limits; a session outside them would make the server reject its whole batch.
constexpr int64_t MIN_START = 946684800;  // 2000-01-01
constexpr uint32_t MIN_SECONDS = 10;
constexpr uint32_t MAX_SECONDS = 7 * 86400;
constexpr int64_t MAX_FUTURE_SECONDS = 300;

int utcOffsetMinutes(const time_t at) {
  struct tm local;
  struct tm utc;
  localtime_r(&at, &local);
  gmtime_r(&at, &utc);
  // Reading the UTC fields as local time shifts the result by exactly the zone offset.
  utc.tm_isdst = local.tm_isdst;
  return std::clamp(static_cast<int>((at - mktime(&utc)) / 60), -720, 840);
}

// Reads one line without its newline. An overlong line comes back empty.
// Returns false at the end of the file; an unfinished last line is not returned.
bool readLine(HalFile& file, char* line, const size_t size) {
  size_t length = 0;
  bool overlong = false;
  char c;
  while (file.read(&c, 1) == 1) {
    if (c == '\n') {
      line[overlong ? 0 : length] = '\0';
      return true;
    }
    if (length + 1 < size) {
      line[length++] = c;
    } else {
      overlong = true;
    }
  }
  return false;
}

bool isDocumentId(const char* document) {
  return strlen(document) == 32 && strspn(document, "0123456789abcdefABCDEF") == 32;
}

// Unique per device: every CrossPoint reader reports the same device id to the server.
void formatSessionId(char* out, const size_t size, const int64_t start, const uint8_t* mac) {
  snprintf(out, size, "%lld-%02x%02x%02x%02x%02x%02x", static_cast<long long>(start), mac[0], mac[1], mac[2], mac[3],
           mac[4], mac[5]);
}

bool saveOffset(const std::string& account, const size_t offset) {
  JsonDocument state;
  state["account"] = account;
  state["offset"] = offset;
  return PersistableStoreBase::writeDocToFile(STATE_PATH, state);
}
}  // namespace

bool readingSessionLog::append(const std::string& document, const int64_t startTime, const uint32_t seconds,
                               const uint16_t startProgressBp, const uint16_t endProgressBp) {
  if (!isDocumentId(document.c_str())) return false;
  char line[MAX_LINE];
  const int length =
      snprintf(line, sizeof(line), "{\"doc\":\"%s\",\"start\":%lld,\"secs\":%lu,\"tz\":%d,\"from\":%u,\"to\":%u}\n",
               document.c_str(), static_cast<long long>(startTime), static_cast<unsigned long>(seconds),
               utcOffsetMinutes(static_cast<time_t>(startTime)), startProgressBp, endProgressBp);
  if (length <= 0 || static_cast<size_t>(length) >= sizeof(line)) return false;

  Storage.mkdir("/.crosspoint");
  HalFile file = Storage.open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND);
  if (!file) {
    LOG_ERR("RSL", "Failed to open the reading session log");
    return false;
  }
  const bool written = file.write(line, static_cast<size_t>(length)) == static_cast<size_t>(length);
  file.close();
  if (!written) LOG_ERR("RSL", "Failed to append a reading session");
  return written;
}

bool readingSessionLog::upload(SyncRequest& sync) {
  if (!Storage.exists(LOG_PATH)) return true;
  HalFile file = Storage.open(LOG_PATH);
  if (!file) return false;

  const std::string account = SyncRequest::account();
  size_t offset = 0;
  {
    JsonDocument state;
    if (PersistableStoreBase::readDocFromFile(STATE_PATH, state) && account == (state["account"] | "")) {
      offset = state["offset"] | static_cast<size_t>(0);
    }
  }
  // A log that shrank was replaced; start over. The server ignores sessions it already holds.
  if (offset > file.size()) offset = 0;

  uint8_t mac[6] = {};
  esp_efuse_mac_get_default(mac);
  const int64_t now = trustedtime::trustedNow();
  const std::string url = KOREADER_STORE.getBaseUrl() + "/api/v1/stats/sessions";
  bool ok = file.seek(offset);
  JsonDocument doc;
  char line[MAX_LINE];
  char sessionId[40];

  for (size_t batch = 0; ok && batch < MAX_BATCHES; ++batch) {
    doc.clear();
    doc["device_id"] = KOReaderSyncClient::DEVICE_ID;
    JsonArray items = doc["items"].to<JsonArray>();
    int64_t starts[BATCH_SIZE];
    size_t count = 0;
    bool endOfLog = false;
    while (count < BATCH_SIZE) {
      if (!readLine(file, line, sizeof(line))) {
        endOfLog = true;
        break;
      }
      JsonDocument entry;
      if (deserializeJson(entry, static_cast<const char*>(line))) continue;
      const char* document = entry["doc"] | "";
      const int64_t start = entry["start"] | static_cast<int64_t>(0);
      const uint32_t seconds = entry["secs"] | static_cast<uint32_t>(0);
      // A session dated in the future was measured with a wrong clock; it stays in the log only.
      if (!isDocumentId(document) || start < MIN_START || seconds < MIN_SECONDS || seconds > MAX_SECONDS ||
          (now > 0 && start + seconds > now + MAX_FUTURE_SECONDS) ||
          std::find(starts, starts + count, start) != starts + count) {
        continue;
      }
      formatSessionId(sessionId, sizeof(sessionId), start, mac);
      JsonObject item = items.add<JsonObject>();
      item["session_id"] = static_cast<const char*>(sessionId);  // const char* makes ArduinoJson copy the text
      item["document"] = document;
      item["started_at"] = start;
      item["ended_at"] = start + seconds;
      item["seconds"] = seconds;
      item["utc_offset_minutes"] = std::clamp(entry["tz"] | 0, -720, 840);
      starts[count++] = start;
    }
    const size_t consumed = file.position();

    if (count > 0) {
      if (!sync.send(url, doc, true)) {
        ok = false;
        break;
      }
      // The server lists what it stored; an older server answers 200 without storing anything.
      const JsonArrayConst accepted = doc["accepted_session_ids"].as<JsonArrayConst>();
      for (size_t i = 0; i < count && ok; ++i) {
        formatSessionId(sessionId, sizeof(sessionId), starts[i], mac);
        ok = false;
        for (JsonVariantConst id : accepted) {
          if (strcmp(id | "", sessionId) == 0) ok = true;
        }
      }
      if (!ok) {
        LOG_ERR("RSL", "Server did not acknowledge reading sessions");
        break;
      }
    }
    if (consumed != offset) {
      offset = consumed;
      ok = saveOffset(account, offset);
    }
    if (endOfLog) break;
  }
  file.close();
  return ok;
}
