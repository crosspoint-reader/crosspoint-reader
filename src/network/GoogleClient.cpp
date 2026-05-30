#include "GoogleClient.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_sntp.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include "CrossPointSettings.h"
#include "WifiCredentialStore.h"

namespace {
constexpr char CREDS_PATH[] = "/.crosspoint/google_creds.json";
constexpr char TOKEN_URL[] = "https://oauth2.googleapis.com/token";
// 7-day window, trimmed fields keep the response a few KB so it fits in RAM.
constexpr char CALENDAR_URL[] =
    "https://www.googleapis.com/calendar/v3/calendars/primary/events"
    "?singleEvents=true&orderBy=startTime&maxResults=25"
    "&fields=items(summary,location,start,end)";
constexpr char TASKS_URL[] =
    "https://tasks.googleapis.com/tasks/v1/lists/@default/tasks"
    "?showCompleted=false&maxResults=25&fields=items(title,notes,due,status)";

constexpr int HTTP_RX_BUF = 2048;
constexpr int HTTP_TX_BUF = 1024;
constexpr int HTTP_TIMEOUT_MS = 20000;
constexpr size_t READ_CHUNK = 1024;
constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 20000;
constexpr int CALENDAR_WINDOW_DAYS = 7;
// Sanity floor for the system clock: 2023-11-14. Below this we assume NTP never
// completed (deep-sleep wake resets the newlib clock), so the 7-day query window
// and every countdown would be garbage.
constexpr time_t MIN_VALID_EPOCH = 1700000000;

// Set for the duration of a syncAll() call so the WiFi wait and HTTP read loops
// can bail out promptly. Only one sync runs at a time, so a namespace-scope
// pointer avoids threading a parameter through every helper.
const volatile bool* g_cancelFlag = nullptr;
bool cancelled() { return g_cancelFlag != nullptr && *g_cancelFlag; }

struct Creds {
  std::string client_id;
  std::string client_secret;
  std::string refresh_token;
};

// --- Time helpers -----------------------------------------------------------

// Days since 1970-01-01 for a civil (proleptic Gregorian) date. Howard
// Hinnant's algorithm; avoids mktime/timegm TZ ambiguity on newlib.
int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

time_t makeUtc(int y, int mo, int d, int h, int mi, int s) {
  return static_cast<time_t>(daysFromCivil(y, mo, d)) * 86400 + h * 3600 + mi * 60 + s;
}

// Parse an RFC3339 timestamp ("2026-05-29T18:30:00+08:00", "...Z", or a bare
// "2026-05-29" date) into a UTC epoch. Returns 0 on parse failure.
time_t parseRfc3339(const char* s) {
  if (!s || strlen(s) < 10) return 0;
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
  if (sscanf(s, "%4d-%2d-%2d", &y, &mo, &d) != 3) return 0;
  if (mo < 1 || mo > 12 || d < 1 || d > 31) return 0;

  // Bare date (all-day): treat as midnight UTC of that day.
  if (s[10] != 'T') return makeUtc(y, mo, d, 0, 0, 0);

  if (sscanf(s + 11, "%2d:%2d:%2d", &h, &mi, &se) < 2) return 0;
  time_t base = makeUtc(y, mo, d, h, mi, se);

  // Locate the timezone designator after the time (skip optional fraction).
  const char* p = s + 11;
  while (*p && *p != 'Z' && *p != '+' && !(p > s + 18 && *p == '-')) p++;
  if (*p == 'Z' || *p == '\0') return base;  // already UTC
  if (*p == '+' || *p == '-') {
    int oh = 0, om = 0;
    if (sscanf(p + 1, "%2d:%2d", &oh, &om) >= 1) {
      const int offset = (oh * 3600 + om * 60) * (*p == '+' ? 1 : -1);
      base -= offset;  // convert local -> UTC
    }
  }
  return base;
}

void formatRfc3339Utc(time_t epoch, char* buf, size_t len) {
  struct tm tmv;
  gmtime_r(&epoch, &tmv);
  snprintf(buf, len, "%04d-%02d-%02dT%02d:%02d:%02dZ", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour,
           tmv.tm_min, tmv.tm_sec);
}

// Percent-encode a query-parameter value (RFC3339 timestamps contain ':' / '+').
std::string urlEncode(const std::string& in) {
  static const char hex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(in.size() * 3);
  for (unsigned char c : in) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 0x0F]);
    }
  }
  return out;
}

// --- HTTP -------------------------------------------------------------------

// Execute one request against a verified-HTTPS endpoint and collect the body.
// Mirrors HttpDownloader's manual open/fetch/read pattern (perform() buffers the
// whole body via callbacks). `bearer` and `body` are optional.
bool httpExec(esp_http_client_method_t method, const std::string& url, const std::string& bearer,
              const std::string& body, std::string& outBody, int& outStatus) {
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.buffer_size = HTTP_RX_BUF;
  config.buffer_size_tx = HTTP_TX_BUF;
  config.timeout_ms = HTTP_TIMEOUT_MS;
  config.crt_bundle_attach = esp_crt_bundle_attach;  // verify against bundled CA roots
  config.method = method;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    LOG_ERR("GOOG", "client init failed");
    return false;
  }

  if (!bearer.empty()) {
    const std::string auth = "Bearer " + bearer;
    esp_http_client_set_header(client, "Authorization", auth.c_str());
  }
  if (method == HTTP_METHOD_POST) {
    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
  }

  esp_err_t err = esp_http_client_open(client, body.size());
  if (err != ESP_OK) {
    LOG_ERR("GOOG", "open failed: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return false;
  }
  if (!body.empty()) {
    const int written = esp_http_client_write(client, body.c_str(), body.size());
    if (written < 0 || static_cast<size_t>(written) != body.size()) {
      LOG_ERR("GOOG", "write failed (%d of %u)", written, (unsigned)body.size());
      esp_http_client_cleanup(client);
      return false;
    }
  }

  esp_http_client_fetch_headers(client);
  outStatus = esp_http_client_get_status_code(client);

  outBody.clear();
  // Heap-allocated read buffer: the sync task's stack is reserved for the TLS
  // handshake, so keep large locals off it (see CLAUDE.md stack-safety rule).
  auto buf = makeUniqueNoThrow<char[]>(READ_CHUNK);
  if (!buf) {
    LOG_ERR("GOOG", "OOM: %u byte read buffer", (unsigned)READ_CHUNK);
    esp_http_client_cleanup(client);
    return false;
  }
  while (true) {
    if (cancelled()) {
      LOG_INF("GOOG", "read aborted (cancel)");
      esp_http_client_cleanup(client);
      return false;
    }
    const int read = esp_http_client_read(client, buf.get(), READ_CHUNK);
    if (read < 0) {
      LOG_ERR("GOOG", "read error");
      esp_http_client_cleanup(client);
      return false;
    }
    if (read == 0) break;
    outBody.append(buf.get(), read);
  }
  esp_http_client_cleanup(client);
  return true;
}

// --- Credentials ------------------------------------------------------------

bool loadCreds(Creds& out) {
  if (!Storage.exists(CREDS_PATH)) {
    LOG_ERR("GOOG", "Missing %s", CREDS_PATH);
    return false;
  }
  const String content = Storage.readFile(CREDS_PATH);
  if (content.isEmpty()) {
    LOG_ERR("GOOG", "Empty %s", CREDS_PATH);
    return false;
  }
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, content.c_str());
  if (err) {
    LOG_ERR("GOOG", "creds parse error: %s", err.c_str());
    return false;
  }
  out.client_id = doc["client_id"] | "";
  out.client_secret = doc["client_secret"] | "";
  out.refresh_token = doc["refresh_token"] | "";
  if (out.client_id.empty() || out.client_secret.empty() || out.refresh_token.empty()) {
    LOG_ERR("GOOG", "creds incomplete");
    return false;
  }
  return true;
}

// --- WiFi + NTP -------------------------------------------------------------

bool connectWifi() {
  if (WiFi.status() == WL_CONNECTED) return true;

  const std::string ssid = WIFI_STORE.getLastConnectedSsid();
  if (ssid.empty()) {
    LOG_ERR("GOOG", "No last-connected SSID saved");
    return false;
  }
  const WifiCredential* cred = WIFI_STORE.findCredential(ssid);
  if (!cred) {
    LOG_ERR("GOOG", "No saved credential for SSID");
    return false;
  }

  LOG_INF("GOOG", "Connecting WiFi to saved network");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), cred->password.c_str());

  const unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    if (cancelled()) {
      LOG_INF("GOOG", "WiFi wait aborted (cancel)");
      return false;
    }
    delay(100);
  }
  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("GOOG", "WiFi connect timed out");
    return false;
  }
  // Persist the network we actually connected to so it survives a reboot.
  // Guard the write: only touch SPIFFS when it actually changed (write throttling).
  if (WIFI_STORE.getLastConnectedSsid() != ssid) {
    WIFI_STORE.setLastConnectedSsid(ssid);
    WIFI_STORE.saveToFile();
  }
  return true;
}

void syncClock() {
  // Keep the DS3231 fresh when present; this also sets the system clock via
  // SNTP. On hardware without an RTC, drive SNTP directly so time(nullptr) is
  // valid for countdown math.
  if (halClock.isAvailable()) {
    halClock.syncFromNTP();
    return;
  }
  configTzTime("UTC0", "pool.ntp.org", "time.nist.gov");
  for (int i = 0; i < 50; i++) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) return;
    delay(100);
  }
  LOG_ERR("GOOG", "NTP sync timed out (no RTC)");
}

// --- Token ------------------------------------------------------------------

bool refreshAccessToken(const Creds& creds, std::string& accessToken) {
  const std::string body = "client_id=" + urlEncode(creds.client_id) +
                           "&client_secret=" + urlEncode(creds.client_secret) +
                           "&refresh_token=" + urlEncode(creds.refresh_token) + "&grant_type=refresh_token";
  std::string resp;
  int status = 0;
  if (!httpExec(HTTP_METHOD_POST, TOKEN_URL, "", body, resp, status)) return false;
  if (status != 200) {
    LOG_ERR("GOOG", "token endpoint status %d", status);
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, resp)) {
    LOG_ERR("GOOG", "token parse error");
    return false;
  }
  accessToken = doc["access_token"] | "";
  if (accessToken.empty()) {
    LOG_ERR("GOOG", "no access_token in response");
    return false;
  }
  return true;
}

// --- Calendar + Tasks -------------------------------------------------------

// Copy only the first comma-separated portion of a location string.
void copyLocationFirstField(const char* src, char* dst, size_t dstLen) {
  if (!src) {
    dst[0] = '\0';
    return;
  }
  const char* comma = strchr(src, ',');
  const size_t n = comma ? static_cast<size_t>(comma - src) : strlen(src);
  const size_t copy = n < dstLen - 1 ? n : dstLen - 1;
  memcpy(dst, src, copy);
  dst[copy] = '\0';
}

bool fetchCalendar(const std::string& token, RemindersData& out) {
  const time_t now = time(nullptr);
  char timeMin[24], timeMax[24];
  formatRfc3339Utc(now, timeMin, sizeof(timeMin));
  formatRfc3339Utc(now + static_cast<time_t>(CALENDAR_WINDOW_DAYS) * 86400, timeMax, sizeof(timeMax));

  std::string url = std::string(CALENDAR_URL) + "&timeMin=" + urlEncode(timeMin) + "&timeMax=" + urlEncode(timeMax);

  std::string resp;
  int status = 0;
  if (!httpExec(HTTP_METHOD_GET, url, token, "", resp, status)) return false;
  if (status != 200) {
    LOG_ERR("GOOG", "calendar status %d", status);
    return false;
  }

  // Filter to just the fields we keep, so the parsed document stays tiny.
  JsonDocument filter;
  JsonObject fi = filter["items"].add<JsonObject>();
  fi["summary"] = true;
  fi["location"] = true;
  fi["start"] = true;
  fi["end"] = true;

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, resp, DeserializationOption::Filter(filter));
  if (err) {
    LOG_ERR("GOOG", "calendar parse error: %s", err.c_str());
    return false;
  }

  for (JsonObject ev : doc["items"].as<JsonArray>()) {
    if (out.count >= REMINDERS_MAX_ITEMS) break;
    CalItem it = {};
    it.is_calendar = true;
    snprintf(it.title, sizeof(it.title), "%s", ev["summary"] | "(no title)");
    copyLocationFirstField(ev["location"] | "", it.location, sizeof(it.location));

    // An all-day event carries only "date" (no "dateTime"). Track that so the
    // renderer shows "ALL DAY <date>" instead of a countdown to UTC midnight.
    JsonObject start = ev["start"];
    const char* startTimed = start["dateTime"];
    const char* startDate = start["date"];
    it.all_day = (startTimed == nullptr) && (startDate != nullptr);
    it.start_epoch = parseRfc3339(startTimed != nullptr ? startTimed : startDate);
    JsonObject end = ev["end"];
    const char* endTimed = end["dateTime"];
    const char* endDate = end["date"];
    it.end_epoch = parseRfc3339(endTimed != nullptr ? endTimed : endDate);

    out.items[out.count++] = it;
  }
  LOG_DBG("GOOG", "calendar: %u items so far", out.count);
  return true;
}

bool fetchTasks(const std::string& token, RemindersData& out) {
  std::string resp;
  int status = 0;
  if (!httpExec(HTTP_METHOD_GET, TASKS_URL, token, "", resp, status)) return false;
  if (status != 200) {
    LOG_ERR("GOOG", "tasks status %d", status);
    return false;
  }

  JsonDocument filter;
  JsonObject fi = filter["items"].add<JsonObject>();
  fi["title"] = true;
  fi["notes"] = true;
  fi["due"] = true;
  fi["status"] = true;

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, resp, DeserializationOption::Filter(filter));
  if (err) {
    LOG_ERR("GOOG", "tasks parse error: %s", err.c_str());
    return false;
  }

  for (JsonObject tk : doc["items"].as<JsonArray>()) {
    if (out.count >= REMINDERS_MAX_ITEMS) break;
    const char* tkStatus = tk["status"] | "needsAction";
    const bool completed = strcmp(tkStatus, "completed") == 0;
    if (completed) continue;  // belt-and-suspenders; we already ask for incomplete only

    CalItem it = {};
    it.is_calendar = false;
    it.completed = false;
    snprintf(it.title, sizeof(it.title), "%s", tk["title"] | "(untitled task)");
    // Tasks only record a due *date* (time is always midnight and ignored), so
    // treat any due as all-day: show the date, not a midnight-UTC countdown.
    const char* due = tk["due"] | (const char*)nullptr;
    it.start_epoch = parseRfc3339(due);
    it.all_day = (due != nullptr);

    // Split notes on newlines into up to REMINDERS_MAX_NOTES sub-items.
    const char* notes = tk["notes"] | "";
    while (*notes && it.note_count < REMINDERS_MAX_NOTES) {
      const char* nl = strchr(notes, '\n');
      const size_t n = nl ? static_cast<size_t>(nl - notes) : strlen(notes);
      if (n > 0) {
        const size_t copy = n < sizeof(it.notes[0]) - 1 ? n : sizeof(it.notes[0]) - 1;
        memcpy(it.notes[it.note_count], notes, copy);
        it.notes[it.note_count][copy] = '\0';
        it.note_count++;
      }
      if (!nl) break;
      notes = nl + 1;
    }

    out.items[out.count++] = it;
  }
  LOG_DBG("GOOG", "tasks: %u items total", out.count);
  return true;
}

}  // namespace

GoogleClient::Result GoogleClient::syncAll(RemindersData& out, const volatile bool* cancel) {
  g_cancelFlag = cancel;
  // Always clear the namespace flag on the way out, whatever path we take.
  struct CancelGuard {
    ~CancelGuard() { g_cancelFlag = nullptr; }
  } cancelGuard;

  Creds creds;
  if (!loadCreds(creds)) return Result::NoCredentials;

  if (cancelled()) return Result::Cancelled;
  if (!connectWifi()) return cancelled() ? Result::Cancelled : Result::WifiFailed;

  syncClock();

  // Tear WiFi down on every early exit below via this helper.
  auto wifiOff = []() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  };

  if (cancelled()) {
    wifiOff();
    return Result::Cancelled;
  }

  // Guard against an unset clock: countdowns and the query window depend on a
  // real epoch (deep-sleep wake resets the system clock and NTP may have failed).
  if (time(nullptr) < MIN_VALID_EPOCH) {
    LOG_ERR("GOOG", "System clock not set; aborting sync");
    wifiOff();
    return Result::ClockUnset;
  }

  std::string token;
  if (!refreshAccessToken(creds, token)) {
    wifiOff();
    return cancelled() ? Result::Cancelled : Result::AuthFailed;
  }

  // Accumulate into a local copy so a partial failure never clobbers the cached
  // list already in `out`.
  RemindersData fresh;
  fresh.clear();
  const bool calOk = fetchCalendar(token, fresh);
  // Sequential teardown is implicit: esp_http_client_cleanup() in httpExec frees
  // each TLS session before the next call, so only one TLS context is live at a
  // time (~35KB peak rather than two stacked).
  const bool tasksOk = cancelled() ? false : fetchTasks(token, fresh);

  wifiOff();

  if (cancelled()) return Result::Cancelled;
  if (!calOk && !tasksOk) return Result::FetchFailed;

  fresh.synced_epoch = time(nullptr);
  fresh.is_stale = false;
  fresh.sortByStart();
  out = fresh;
  out.saveToFile();

  LOG_INF("GOOG", "Sync OK: %u items", out.count);
  return Result::OK;
}

const char* GoogleClient::resultName(Result r) {
  switch (r) {
    case Result::OK:
      return "OK";
    case Result::NoCredentials:
      return "NoCredentials";
    case Result::WifiFailed:
      return "WifiFailed";
    case Result::ClockUnset:
      return "ClockUnset";
    case Result::AuthFailed:
      return "AuthFailed";
    case Result::FetchFailed:
      return "FetchFailed";
    case Result::Cancelled:
      return "Cancelled";
  }
  return "?";
}
