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

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

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
    "?showCompleted=false&maxResults=25&fields=items(id,title,notes,due,status)";
constexpr char TASKS_COMPLETE_URL_PREFIX[] = "https://tasks.googleapis.com/tasks/v1/lists/@default/tasks/";
// Distance Matrix: one origin (home) to many destinations in a single request.
// duration_in_traffic needs a future departure_time, supplied per-call.
constexpr char DISTANCE_MATRIX_URL[] =
    "https://maps.googleapis.com/maps/api/distancematrix/json?mode=transit&units=metric";
// Cap the destinations per request so the response stays within HTTP_RX_BUF and
// the parsed document stays small. Comfortably above any one-page reminder list.
constexpr int MAPS_MAX_DESTINATIONS = 10;

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

// Extracts the UTC offset encoded in an RFC3339 dateTime string and converts it to the
// clockUtcOffsetQ biased encoding (48 = UTC+0, 80 = UTC+8). Returns false if no explicit
// offset is present (bare date or malformed). DST-correct since it reads the actual offset
// baked into the timestamp rather than relying on an IANA timezone name lookup table.
bool extractRfc3339OffsetQ(const char* s, uint8_t& offsetQ) {
  if (!s || s[10] != 'T') return false;
  const char* p = s + 11;
  while (*p && *p != 'Z' && *p != '+' && !(p > s + 18 && *p == '-')) p++;
  if (*p == 'Z') {
    offsetQ = 48;
    return true;
  }
  if (*p == '+' || *p == '-') {
    int oh = 0, om = 0;
    if (sscanf(p + 1, "%2d:%2d", &oh, &om) >= 1) {
      int quarters = oh * 4 + om / 15;
      if (*p == '-') quarters = -quarters;
      const int biased = 48 + quarters;
      if (biased >= 0 && biased <= 104) {
        offsetQ = static_cast<uint8_t>(biased);
        return true;
      }
    }
  }
  return false;
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
              const std::string& body, std::string& outBody, int& outStatus, const char* contentType = nullptr) {
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
  if (!body.empty()) {
    const char* ct =
        contentType ? contentType : (method == HTTP_METHOD_POST ? "application/x-www-form-urlencoded" : nullptr);
    if (ct) esp_http_client_set_header(client, "Content-Type", ct);
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

// Try to bring up WiFi against a single saved network. Returns true on connect,
// false on timeout/cancel. Caller owns the candidate ordering.
bool tryConnectSsid(const std::string& ssid, const std::string& password) {
  LOG_INF("GOOG", "Connecting WiFi to saved network");
  WiFi.begin(ssid.c_str(), password.c_str());

  const unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    if (cancelled()) {
      LOG_INF("GOOG", "WiFi wait aborted (cancel)");
      return false;
    }
    delay(100);
  }
  return WiFi.status() == WL_CONNECTED;
}

// Returns the best (strongest) RSSI for `ssid` across all scan entries, or
// INT32_MIN if the SSID was not seen in the scan. A network can appear multiple
// times (mesh/repeaters); we want the strongest.
int32_t bestRssiInScan(int scanCount, const std::string& ssid) {
  int32_t best = INT32_MIN;
  for (int i = 0; i < scanCount; i++) {
    if (ssid == WiFi.SSID(i).c_str()) {
      const int32_t rssi = WiFi.RSSI(i);
      if (rssi > best) best = rssi;
    }
  }
  return best;
}

bool connectWifi() {
  if (WiFi.status() == WL_CONNECTED) return true;

  if (WIFI_STORE.getCredentials().empty()) {
    LOG_ERR("GOOG", "No saved WiFi networks to connect to");
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  // Scan for what's actually in range, then connect to the strongest *saved*
  // network that's present. This is the key to reliable wake-from-sleep sync:
  // the old behaviour blindly tried the last-connected SSID first (a full 20s
  // timeout) even when that network was long gone, never noticing a different
  // saved network sitting right there. An active scan lets us skip absent
  // networks entirely and pick the readily-available one. The scan runs a few
  // seconds; we are on the off-loop sync task so blocking is fine.
  const int scanCount = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/true);

  // Ordered candidate list. First: saved networks seen in the scan, strongest
  // signal first. Then: any saved networks NOT seen (covers hidden SSIDs the
  // scan may miss, and a totally failed scan) in stored order, so we never do
  // worse than the previous saved-only behaviour.
  struct Candidate {
    std::string ssid;
    int32_t rssi;
  };
  std::vector<Candidate> present;
  std::vector<std::string> absent;
  present.reserve(WIFI_STORE.getCredentials().size());
  absent.reserve(WIFI_STORE.getCredentials().size());
  for (const WifiCredential& c : WIFI_STORE.getCredentials()) {
    const int32_t rssi = scanCount > 0 ? bestRssiInScan(scanCount, c.ssid) : INT32_MIN;
    if (rssi != INT32_MIN) {
      present.push_back({c.ssid, rssi});
    } else {
      absent.push_back(c.ssid);
    }
  }
  if (scanCount >= 0) WiFi.scanDelete();
  std::sort(present.begin(), present.end(), [](const Candidate& a, const Candidate& b) { return a.rssi > b.rssi; });

  std::vector<std::string> candidates;
  candidates.reserve(present.size() + absent.size());
  for (const Candidate& c : present) candidates.push_back(c.ssid);
  for (const std::string& s : absent) candidates.push_back(s);

  LOG_INF("GOOG", "WiFi scan: %d AP(s), %u saved in range", scanCount < 0 ? 0 : scanCount, (unsigned)present.size());

  for (const std::string& ssid : candidates) {
    if (cancelled()) return false;
    const WifiCredential* cred = WIFI_STORE.findCredential(ssid);
    if (!cred) continue;
    if (tryConnectSsid(ssid, cred->password)) {
      // Persist the network we actually connected to so the next sync takes the
      // fast path. setLastConnectedSsid() guards + saves only on change.
      WIFI_STORE.setLastConnectedSsid(ssid);
      return true;
    }
    WiFi.disconnect();
  }
  LOG_ERR("GOOG", "WiFi connect failed (tried %u saved network(s))", (unsigned)candidates.size());
  return false;
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

  bool tzSynced = false;
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

    // Detect timezone from the first event that carries an explicit UTC offset in its
    // dateTime. This is DST-correct (reads the actual baked-in offset, not an IANA name).
    if (!tzSynced && SETTINGS.gcalTimezoneSync && startTimed) {
      uint8_t offsetQ = 0;
      if (extractRfc3339OffsetQ(startTimed, offsetQ)) {
        if (offsetQ != SETTINGS.clockUtcOffsetQ) {
          LOG_INF("GOOG", "Timezone from GCal offsetQ=%u (was %u)", offsetQ, SETTINGS.clockUtcOffsetQ);
          SETTINGS.clockUtcOffsetQ = offsetQ;
          SETTINGS.saveToFile();
        }
        tzSynced = true;
      }
    }

    out.items[out.count++] = it;
  }
  LOG_DBG("GOOG", "calendar: %u items so far", out.count);
  return true;
}

// Populate travel_secs for timed calendar items that carry a destination, using
// the Google Distance Matrix API (one origin = home address, many destinations
// per request). No-op unless SETTINGS.mapsApiKey is set. Best-effort: any failure
// here leaves travel_secs at 0 and never fails the overall sync — the renderer
// then shows the plain start time instead of a "LEAVE BY" banner.
void fetchTravelTimes(RemindersData& out) {
  const std::string apiKey = SETTINGS.mapsApiKey;
  const std::string origin = SETTINGS.homeAddress;
  if (apiKey.empty()) {
    LOG_DBG("GOOG", "travel times: mapsApiKey not set, skipping");
    return;
  }
  if (origin.empty()) {
    LOG_DBG("GOOG", "travel times: homeAddress not set, skipping");
    return;
  }

  const time_t now = time(nullptr);

  // Indices of items needing a lookup: future, timed, calendar, with a location.
  uint8_t pending[REMINDERS_MAX_ITEMS];
  uint8_t nPending = 0;
  for (uint8_t i = 0; i < out.count; i++) {
    const CalItem& it = out.items[i];
    const bool qualifies = it.is_calendar && !it.all_day && it.start_epoch > now && it.location[0] != '\0';
    LOG_DBG("GOOG", "item[%u] \"%s\" cal=%d allday=%d future=%d loc=\"%s\" -> %s", i, it.title, it.is_calendar,
            it.all_day, it.start_epoch > now, it.location, qualifies ? "QUEUED" : "skip");
    if (qualifies) pending[nPending++] = i;
  }
  if (nPending == 0) {
    LOG_DBG("GOOG", "travel times: no qualifying items (need future timed calendar event with location)");
    return;
  }

  char depBuf[16];
  snprintf(depBuf, sizeof(depBuf), "%ld", static_cast<long>(now));

  for (uint8_t base = 0; base < nPending; base += MAPS_MAX_DESTINATIONS) {
    if (cancelled()) return;
    const uint8_t chunk = static_cast<uint8_t>(std::min<int>(MAPS_MAX_DESTINATIONS, nPending - base));

    // Destinations joined by '|' (Distance Matrix's multi-destination separator),
    // then percent-encoded as a single query value.
    std::string dests;
    for (uint8_t j = 0; j < chunk; j++) {
      if (j != 0) dests += "|";
      dests += out.items[pending[base + j]].location;
    }

    const std::string url = std::string(DISTANCE_MATRIX_URL) + "&departure_time=" + depBuf +
                            "&origins=" + urlEncode(origin) + "&destinations=" + urlEncode(dests) +
                            "&key=" + urlEncode(apiKey);

    std::string resp;
    int status = 0;
    // Maps uses the URL key, not a bearer token, so pass an empty bearer.
    LOG_DBG("GOOG", "distance matrix: querying %u dest(s)", chunk);
    if (!httpExec(HTTP_METHOD_GET, url, "", "", resp, status) || status != 200) {
      LOG_ERR("GOOG", "distance matrix HTTP status %d", status);
      continue;  // best-effort: skip this chunk, keep the rest of the sync
    }

    // Filter to rows[].elements[].{status,duration,duration_in_traffic}.
    JsonDocument filter;
    JsonObject rowFi = filter["rows"].add<JsonObject>();
    JsonObject elFi = rowFi["elements"].add<JsonObject>();
    elFi["status"] = true;
    elFi["duration"] = true;
    elFi["duration_in_traffic"] = true;

    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, resp, DeserializationOption::Filter(filter));
    if (err) {
      LOG_ERR("GOOG", "distance matrix parse error: %s", err.c_str());
      continue;
    }

    // Single origin → results live in rows[0].elements, aligned with `dests`.
    uint8_t j = 0;
    for (JsonObject el : doc["rows"][0]["elements"].as<JsonArray>()) {
      if (j >= chunk) break;
      const char* st = el["status"] | "";
      if (strcmp(st, "OK") == 0) {
        // Prefer traffic-aware duration when the key/billing returns it.
        int32_t secs = el["duration_in_traffic"]["value"] | 0;  // cppcheck-suppress badBitmaskCheck
        if (secs <= 0) secs = el["duration"]["value"] | 0;      // cppcheck-suppress badBitmaskCheck
        LOG_DBG("GOOG", "element[%u] OK, travel_secs=%ld", j, static_cast<long>(secs));
        if (secs > 0) out.items[pending[base + j]].travel_secs = secs;
      } else {
        LOG_ERR("GOOG", "element[%u] status=\"%s\" (REQUEST_DENIED=bad key/billing, NOT_FOUND=bad location)", j, st);
      }
      j++;
    }
  }
  LOG_DBG("GOOG", "travel times: %u destinations queried", nPending);
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
  fi["id"] = true;
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
    snprintf(it.task_id, sizeof(it.task_id), "%s", tk["id"] | "");
    // Tasks only record a due *date* (time is always midnight and ignored), so
    // treat any due as all-day: show the date, not a midnight-UTC countdown.
    const char* due = tk["due"] | (const char*)nullptr;  // cppcheck-suppress badBitmaskCheck
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

// PATCH a single Google Task to status=completed. Connects WiFi, refreshes the
// token, issues the PATCH, then disconnects. On success sets item.completed=true
// and saves the cache; on any failure `out` is left untouched.
bool completeTask(const std::string& token, CalItem& item) {
  if (item.task_id[0] == '\0') {
    LOG_ERR("GOOG", "completeTask: empty task_id");
    return false;
  }
  const std::string url = std::string(TASKS_COMPLETE_URL_PREFIX) + item.task_id;
  std::string resp;
  int status = 0;
  if (!httpExec(HTTP_METHOD_PATCH, url, token, "{\"status\":\"completed\"}", resp, status, "application/json")) {
    return false;
  }
  if (status != 200 && status != 204) {
    LOG_ERR("GOOG", "completeTask PATCH status %d", status);
    return false;
  }
  item.completed = true;
  LOG_INF("GOOG", "Task completed: %s", item.title);
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
  // Best-effort travel-time enrichment for located calendar events. Runs only
  // when a Maps API key is configured; never affects the sync result.
  if (calOk && !cancelled()) fetchTravelTimes(fresh);
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

GoogleClient::Result GoogleClient::markTaskComplete(uint8_t itemIndex, RemindersData& data,
                                                    const volatile bool* cancel) {
  g_cancelFlag = cancel;
  struct CancelGuard {
    ~CancelGuard() { g_cancelFlag = nullptr; }
  } guard;

  if (itemIndex >= data.count) return Result::FetchFailed;
  CalItem& item = data.items[itemIndex];
  if (item.is_calendar || item.task_id[0] == '\0') return Result::FetchFailed;

  Creds creds;
  if (!loadCreds(creds)) return Result::NoCredentials;
  if (cancelled()) return Result::Cancelled;
  if (!connectWifi()) return cancelled() ? Result::Cancelled : Result::WifiFailed;

  auto wifiOff = []() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  };

  if (cancelled()) {
    wifiOff();
    return Result::Cancelled;
  }

  std::string token;
  if (!refreshAccessToken(creds, token)) {
    wifiOff();
    return cancelled() ? Result::Cancelled : Result::AuthFailed;
  }

  const bool ok = completeTask(token, item);
  wifiOff();

  if (!ok) return Result::FetchFailed;

  data.saveToFile();
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
