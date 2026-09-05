#include "PluginEvents.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <time.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "PluginHttp.h"
#include "PluginLocations.h"
#include "components/UITheme.h"

namespace {

constexpr const char* kEventNames[] = {"reader.open", "reader.exit", "book.downloaded", "sleep.enter"};
static_assert(sizeof(kEventNames) / sizeof(kEventNames[0]) == static_cast<size_t>(pluginevents::Event::COUNT),
              "event name table out of sync");

constexpr const char* kOutboxName = "/events.jsonl";
// Drop-oldest wholesale: a plugin that is never drained must not grow a file
// forever, and by the time 4KB of events piled up the old ones describe stale
// state anyway.
constexpr size_t MAX_OUTBOX_BYTES = 4 * 1024;
constexpr size_t MAX_MANIFEST_SIZE = 8 * 1024;
constexpr size_t MAX_EVENT_LINE = 512;
// Event handler responses are acknowledgements, not content.
constexpr size_t MAX_EVENT_RESPONSE = 8 * 1024;

// Static subscription table: one slot per installed plugin that declares an
// "events" section. Rebuilt by refreshSubscriptions(); sized for a full
// plugin list screen, not a marketplace.
constexpr size_t MAX_EVENT_PLUGINS = 8;
struct Subscriber {
  char name[24] = {0};      // plugin folder name; "" = empty slot
  char dir[64] = {0};       // "<root>/<name>"
  uint8_t mask = 0;         // bit per Event
  uint8_t connectMask = 0;  // events whose handler declares "connect": true
};
Subscriber subscribers[MAX_EVENT_PLUGINS];

uint8_t eventBit(const pluginevents::Event e) { return static_cast<uint8_t>(1u << static_cast<uint8_t>(e)); }

int eventFromName(const char* name) {
  for (size_t i = 0; i < static_cast<size_t>(pluginevents::Event::COUNT); i++) {
    if (strcmp(kEventNames[i], name) == 0) return static_cast<int>(i);
  }
  return -1;
}

std::string outboxPath(const Subscriber& sub) { return std::string(sub.dir) + kOutboxName; }

}  // namespace

namespace pluginevents {

void refreshSubscriptions() {
  for (auto& sub : subscribers) sub = Subscriber{};

  size_t slot = 0;
  for (const auto& entry : PluginLocations::scanPlugins()) {
    if (!entry.hasDevice) continue;
    if (slot >= MAX_EVENT_PLUGINS) {
      LOG_ERR("PEVT", "subscription table full; ignoring %s", entry.name.c_str());
      break;
    }
    std::string raw;
    if (!Storage.readFileToString("PEVT", entry.dir + "/device.json", MAX_MANIFEST_SIZE, raw)) continue;
    // Filtered parse: only the events section, so a big manifest costs a few
    // hundred bytes here instead of a full document.
    JsonDocument filter;
    filter["events"] = true;
    JsonDocument doc;
    if (deserializeJson(doc, raw, DeserializationOption::Filter(filter)) != DeserializationError::Ok) continue;
    uint8_t mask = 0;
    uint8_t connectMask = 0;
    for (JsonPairConst kv : doc["events"].as<JsonObjectConst>()) {
      const int e = eventFromName(kv.key().c_str());
      if (e < 0) {
        LOG_DBG("PEVT", "%s: unknown event '%s' ignored", entry.name.c_str(), kv.key().c_str());
        continue;
      }
      mask |= static_cast<uint8_t>(1u << e);
      if (kv.value()["connect"] | false) connectMask |= static_cast<uint8_t>(1u << e);
    }
    if (mask == 0) continue;
    Subscriber& sub = subscribers[slot++];
    strncpy(sub.name, entry.name.c_str(), sizeof(sub.name) - 1);
    strncpy(sub.dir, entry.dir.c_str(), sizeof(sub.dir) - 1);
    sub.mask = mask;
    sub.connectMask = connectMask;
    LOG_DBG("PEVT", "%s subscribes mask=0x%02x", sub.name, sub.mask);
  }
}

bool anySubscriber(const Event e) {
  for (const auto& sub : subscribers) {
    if (sub.name[0] != '\0' && (sub.mask & eventBit(e))) return true;
  }
  return false;
}

bool wantsConnect(const Event e) {
  for (const auto& sub : subscribers) {
    if (sub.name[0] == '\0' || !(sub.connectMask & eventBit(e))) continue;
    if (Storage.exists(outboxPath(sub).c_str())) return true;
  }
  return false;
}

void emit(const Event e, const Var* vars, const size_t varCount) {
  if (!anySubscriber(e)) return;

  // One line: {"e":"reader.exit","ts":1734212345,"vars":{"book":"...","percent":"74"}}
  JsonDocument doc;
  doc["e"] = kEventNames[static_cast<size_t>(e)];
  // Best-effort unix time: 0 when the clock was never set (no RTC, no NTP yet).
  doc["ts"] = static_cast<long long>(time(nullptr));
  JsonObject varsObj = doc["vars"].to<JsonObject>();
  for (size_t i = 0; i < varCount; i++) {
    varsObj[vars[i].key] = vars[i].value;
  }
  std::string line;
  line.reserve(160);
  serializeJson(doc, line);
  line += '\n';
  if (line.size() > MAX_EVENT_LINE) {
    LOG_ERR("PEVT", "event line too large (%u); dropped", static_cast<unsigned>(line.size()));
    return;
  }

  for (const auto& sub : subscribers) {
    if (sub.name[0] == '\0' || !(sub.mask & eventBit(e))) continue;
    const std::string path = outboxPath(sub);
    HalFile file = Storage.open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND);
    if (!file || !file.isOpen()) {
      LOG_ERR("PEVT", "%s: outbox open failed", sub.name);
      continue;
    }
    if (file.fileSize() > MAX_OUTBOX_BYTES) {
      // Drop-oldest wholesale (see MAX_OUTBOX_BYTES).
      file.close();  // explicit: remove follows on the same path
      Storage.remove(path.c_str());
      file = Storage.open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND);
      if (!file || !file.isOpen()) continue;
      LOG_DBG("PEVT", "%s: outbox over cap, dropped", sub.name);
    }
    file.write(reinterpret_cast<const uint8_t*>(line.data()), line.size());
    file.flush();
  }
}

namespace {

// The manifest subset a drain needs: the handlers plus the token/config/auth
// vocabulary shared with the catalog browser.
struct DrainManifest {
  std::string tokenFile, tokenPath, configFile;
  std::string authType, authTokenPath;
  pluginhttp::RequestSpec authReq;
  struct Handler {
    int event = -1;
    pluginhttp::RequestSpec req;
    std::string toast;
    // "download" variant: the response streams to `dest` on SD (e.g. a fresh
    // sleep image) instead of being read as an acknowledgement.
    std::string dest;
    bool isDownload() const { return !dest.empty(); }
  };
  std::vector<Handler> handlers;
  bool hasPasswordGrant() const { return authType == "password" && !authReq.url.empty(); }
};

bool loadDrainManifest(const Subscriber& sub, DrainManifest& out) {
  std::string raw;
  if (!Storage.readFileToString("PEVT", std::string(sub.dir) + "/device.json", MAX_MANIFEST_SIZE, raw)) return false;
  // Filtered parse: the drain runs at the heap-worst moments (sleep entry,
  // web session), so only the sections it reads are materialized.
  JsonDocument filter;
  filter["token"] = true;
  filter["config"] = true;
  filter["auth"] = true;
  filter["events"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, raw, DeserializationOption::Filter(filter)) != DeserializationError::Ok) return false;

  out.tokenFile = doc["token"]["file"] | "";
  out.tokenPath = doc["token"]["path"] | "token";
  out.configFile = doc["config"]["file"] | "";
  JsonVariantConst auth = doc["auth"];
  out.authType = auth["type"] | "device_code";
  pluginhttp::readRequest(auth["request"], "POST", out.authReq);
  out.authTokenPath = auth["token_path"] | "access_token";

  out.handlers.reserve(2);
  for (JsonPairConst kv : doc["events"].as<JsonObjectConst>()) {
    const int e = eventFromName(kv.key().c_str());
    if (e < 0) continue;
    DrainManifest::Handler h;
    h.event = e;
    JsonVariantConst dl = kv.value()["download"];
    if (dl["url"].as<const char*>()) {
      pluginhttp::readRequest(dl, "GET", h.req);
      h.dest = dl["dest"] | "";
      if (h.dest.empty()) continue;  // a download without a destination is meaningless
    } else {
      pluginhttp::readRequest(kv.value()["request"], "POST", h.req);
    }
    h.toast = kv.value()["toast"] | "";
    if (!h.req.url.empty()) out.handlers.push_back(std::move(h));
  }
  return !out.handlers.empty();
}

// {token}, {cfg.*}, {meta.*}, and {event.*} from the queued line's vars
// object. `config` and `meta` hold pre-built patterns ("{cfg.KEY}" /
// "{meta.KEY}") so the keys are not re-concatenated for every template.
std::string drainSubstituted(std::string tpl, const std::string& token, const pluginhttp::Headers& config,
                             const pluginhttp::Headers& meta, JsonVariantConst vars, const long long ts) {
  pluginhttp::substituteAll(tpl, "{token}", token);
  for (const auto& kv : config) pluginhttp::substituteAll(tpl, kv.first.c_str(), kv.second);
  for (const auto& kv : meta) pluginhttp::substituteAll(tpl, kv.first.c_str(), kv.second);
  char tsBuf[16];
  snprintf(tsBuf, sizeof(tsBuf), "%lld", ts);
  pluginhttp::substituteAll(tpl, "{event.ts}", tsBuf);
  for (JsonPairConst kv : vars.as<JsonObjectConst>()) {
    pluginhttp::substituteAll(tpl, (std::string("{event.") + kv.key().c_str() + "}").c_str(),
                              pluginhttp::variantToString(kv.value()));
  }
  return tpl;
}

// Replays one queued line. True = delivered (drop the line); false = transport
// or auth failure (keep it for the next drain). A line with no matching
// handler counts as delivered so a manifest edit can't wedge the queue.
// `token` is shared across the drain: a 401-minted refresh persists to the
// remaining lines instead of re-minting per line.
bool deliverLine(const DrainManifest& mf, const std::string& lineText, std::string& token,
                 const pluginhttp::Headers& config, GfxRenderer* renderer) {
  JsonDocument doc;
  if (deserializeJson(doc, lineText) != DeserializationError::Ok) return true;  // corrupt line: drop
  const int e = eventFromName(doc["e"] | "");
  const DrainManifest::Handler* handler = nullptr;
  for (const auto& h : mf.handlers) {
    if (h.event == e) {
      handler = &h;
      break;
    }
  }
  if (!handler) return true;

  JsonVariantConst vars = doc["vars"];
  const long long ts = doc["ts"] | 0LL;

  // Book-scoped events expose the book's plugin sidecar ("<book>.meta.json",
  // flat fields written at download time) as {meta.*} variables, e.g. a
  // service book id for a sync handler.
  pluginhttp::Headers meta;
  const char* book = vars["book"] | "";
  if (book[0] != '\0') {
    pluginhttp::loadConfigFile(std::string(book) + ".meta.json", meta);
    for (auto& kv : meta) kv.first = "{meta." + kv.first + "}";
  }

  const auto run = [&](const std::string& tok) {
    pluginhttp::Headers headers;
    headers.reserve(handler->req.headers.size());
    for (const auto& h : handler->req.headers) {
      headers.emplace_back(h.first, drainSubstituted(h.second, tok, config, meta, vars, ts));
    }
    if (handler->isDownload()) {
      const std::string dest = drainSubstituted(handler->dest, tok, config, meta, vars, ts);
      // Substituted fields must not climb out of the tree (same guard as the
      // catalog sidecar writer).
      if (dest.empty() || dest[0] != '/' || dest.find("..") != std::string::npos) {
        LOG_ERR("PEVT", "unsafe download dest rejected: %s", dest.c_str());
        return 200;  // treat as delivered: retrying can never fix the manifest
      }
      // Generous for images (a 4-bit 800x480 BMP is ~192KB), still bounded.
      constexpr size_t MAX_EVENT_DOWNLOAD = 1024 * 1024;
      // Stream to a sibling temp and swap it in only on a clean 2xx, so a
      // 404/500 error body can never replace an existing dest (e.g. /sleep.bmp).
      const std::string tmp = dest + ".part";
      const int st = pluginhttp::requestToFile(
          nullptr, drainSubstituted(handler->req.url, tok, config, meta, vars, ts), handler->req.method,
          drainSubstituted(handler->req.body, tok, config, meta, vars, ts), headers, tmp.c_str(), MAX_EVENT_DOWNLOAD);
      if (st >= 200 && st < 300) {
        Storage.remove(dest.c_str());  // rename won't overwrite an existing file
        if (!Storage.rename(tmp.c_str(), dest.c_str())) Storage.remove(tmp.c_str());
      } else {
        Storage.remove(tmp.c_str());
      }
      return st;
    }
    std::string response;
    return pluginhttp::request(nullptr, drainSubstituted(handler->req.url, tok, config, meta, vars, ts),
                               handler->req.method, drainSubstituted(handler->req.body, tok, config, meta, vars, ts),
                               headers, response, MAX_EVENT_RESPONSE);
  };

  int status = run(token);
  // A password-grant token expires; on 401/403 mint a fresh one and retry once.
  if ((status == 401 || status == 403) && mf.hasPasswordGrant()) {
    std::string minted;
    if (pluginhttp::mintPasswordToken(nullptr, drainSubstituted(mf.authReq.url, token, config, meta, vars, ts),
                                      mf.authReq.method,
                                      drainSubstituted(mf.authReq.body, token, config, meta, vars, ts),
                                      mf.authReq.headers, mf.authTokenPath, minted)) {
      pluginhttp::saveTokenToFile(mf.tokenFile, mf.tokenPath, minted);
      token = minted;
      status = run(token);
    }
  }
  if (status < 200 || status >= 300) return false;

  if (renderer && !handler->toast.empty()) {
    GUI.drawPopup(*renderer, drainSubstituted(handler->toast, token, config, meta, vars, ts).c_str());
  }
  return true;
}

}  // namespace

void drain(GfxRenderer* renderer, const size_t maxEvents) {
  size_t budget = maxEvents;
  for (const auto& sub : subscribers) {
    if (budget == 0) break;
    if (sub.name[0] == '\0') continue;
    const std::string path = outboxPath(sub);
    if (!Storage.exists(path.c_str())) continue;

    std::string raw;
    if (!Storage.readFileToString("PEVT", path, MAX_OUTBOX_BYTES + MAX_EVENT_LINE, raw)) continue;

    DrainManifest mf;
    if (!loadDrainManifest(sub, mf)) {
      // Subscribed but no runnable handlers (JS-only consumer, or manifest
      // edited away): the queue would never advance, so clear it.
      Storage.remove(path.c_str());
      continue;
    }
    std::string token;
    pluginhttp::Headers config;
    pluginhttp::loadTokenFromFile(mf.tokenFile, mf.tokenPath, token);
    pluginhttp::loadConfigFile(mf.configFile, config);
    // Pre-build the substitution patterns (see drainSubstituted).
    for (auto& kv : config) kv.first = "{cfg." + kv.first + "}";

    // Walk lines; stop at the first failure so order is preserved.
    size_t pos = 0;
    bool stalled = false;
    while (pos < raw.size() && budget > 0 && !stalled) {
      size_t nl = raw.find('\n', pos);
      if (nl == std::string::npos) nl = raw.size();
      const std::string lineText = raw.substr(pos, nl - pos);
      if (!lineText.empty()) {
        if (deliverLine(mf, lineText, token, config, renderer)) {
          budget--;
        } else {
          stalled = true;
          break;
        }
      }
      pos = nl + 1;
    }

    if (pos >= raw.size() && !stalled) {
      Storage.remove(path.c_str());
    } else if (pos > 0) {
      // Rewrite the unprocessed tail so delivered events are not replayed.
      const std::string tail = raw.substr(pos);
      HalFile file;
      if (Storage.openFileForWrite("PEVT", path, file)) {
        file.write(reinterpret_cast<const uint8_t*>(tail.data()), tail.size());
        file.flush();
      }
    }
    LOG_DBG("PEVT", "%s: drained (stalled=%d)", sub.name, stalled);
  }
}

}  // namespace pluginevents
