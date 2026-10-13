#include "CrossPointWebServer.h"

#include <ArduinoJson.h>
#include <BoardConfig.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <HalClock.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <HalSystem.h>  // feedWatchdog()
#include <Logging.h>
#include <Memory.h>
#include <Txt.h>
#include <WiFi.h>
#include <Xtc.h>
#include <XteinkDetect.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <uri/UriBraces.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>

#include "BookRelocation.h"
#include "BootDiagnostics.h"
#include "ChunkedResponse.h"
#include "CrossPointSettings.h"
#include "FontInstaller.h"
#include "GlobalBookmarkIndex.h"
#include "HttpFileStreamer.h"
#include "OpdsServerStore.h"
#include "ReadingStats.h"
#include "SdCardFontGlobals.h"
#include "SdCardFontRegistry.h"
#include "SettingsList.h"
#include "SystemStatus.h"
#include "UploadStallTolerance.h"
#include "WebAssets.h"
#include "WebDAVHandler.h"
#include "WebPathGuard.h"
#include "WifiCredentialStore.h"
#include "html/WebSetupPageHtml.generated.h"
#include "network/HttpDownloader.h"
#include "util/FontManifestReader.h"
#include "util/PluginLocations.h"

// Log free heap + max contiguous block at a named point.
#define LOG_WEB_MEM(tag)                                                                             \
  LOG_DBG("WEB", "[MEM] %s — free=%lu maxAlloc=%lu", (tag), (unsigned long)esp_get_free_heap_size(), \
          (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT))

// Minimum free heap to attempt serving a response. Below this threshold the
// WebServer library's internal malloc(11) for chunked chunk-size headers fails
// silently, producing malformed HTTP that the browser rejects.
// Minimum maxAlloc to serve a JSON response (sendJson malloc + _prepareHeader String).
static constexpr uint32_t MIN_HEAP_FOR_JSON = 8192;
// Minimum maxAlloc to serve a gzip'd page: the setup page via send_P, or a
// bundle page streamed from the card through a 4KB buffer (WebAssets::serve).
// send_P itself needs no payload heap, but _prepareHeader's String + the two
// sendHeader RequestArgument nodes + the chunk-size malloc(11) add ~1KB of
// small allocs that fragment badly on an already-tight heap. The log shows
// root_enter at maxAlloc=9716 causing catastrophic fragmentation (exit=2036),
// so we require 12KB headroom before attempting a large static file.
static constexpr uint32_t MIN_HEAP_FOR_HTML = 12288;

static bool rejectIfLowMemory(WebServer* server, uint32_t minAlloc = MIN_HEAP_FOR_JSON) {
  const uint32_t avail = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
  if (avail >= minAlloc) {
    return false;
  }
  LOG_DBG("WEB", "Low memory — rejecting request (maxAlloc=%lu, need=%lu)", (unsigned long)avail,
          (unsigned long)minAlloc);
  server->sendHeader("Retry-After", "5");
  server->send(503, "application/json", "{\"error\":\"low memory\"}");
  return true;
}

namespace {
using ProtectedPaths::Target;

// Whether a request may reach into dot-folders: only once the user has chosen to see
// hidden files, the same switch that decides whether the listing shows them.
bool hiddenAllowed() { return SETTINGS.showHiddenFiles != 0; }

constexpr uint16_t UDP_PORTS[] = {54982, 48123, 39001, 44044, 59678};
constexpr uint16_t LOCAL_UDP_PORT = 8134;

#ifndef FONT_MANIFEST_URL
#define FONT_MANIFEST_URL "https://raw.githubusercontent.com/jpirnay/witchhunt-reader/master/assets/sd-fonts/fonts.json"
#endif

// Static pointer for WebSocket callback (WebSocketsServer requires C-style callback)
CrossPointWebServer* wsInstance = nullptr;

// WebSocket upload state
FsFile wsUploadFile;
String wsUploadFileName;
String wsUploadPath;
size_t wsUploadSize = 0;
size_t wsUploadReceived = 0;
unsigned long wsUploadStartTime = 0;
bool wsUploadInProgress = false;
uint8_t wsUploadClientNum = 255;  // 255 = no active upload client
size_t wsLastProgressSent = 0;
String wsLastCompleteName;
size_t wsLastCompleteSize = 0;
unsigned long wsLastCompleteAt = 0;

// Last successful HTTP /delete request (reported alongside the WS upload status)
String lastDeleteName;
size_t lastDeleteCount = 0;
unsigned long lastDeleteAt = 0;

// A cover sidecar ("book.jpg" beside "book.epub") was written: the book's thumbnails were drawn from
// the old cover. They are rebuilt only when the sidecar is newer than them, which a reader without a
// set clock cannot tell, and the single-height thumb_<H>.bmp some Home themes draw is never compared
// at all (docs/sidecar-files.md). Deleting them makes every view draw the new cover.
void dropThumbnailsForCover(const String& imagePath) {
  static constexpr const char* kCoverExtensions[] = {".jpg", ".jpeg", ".png", ".bmp"};
  static constexpr const char* kBookExtensions[] = {".epub", ".xtc", ".xtch", ".txt", ".md"};
  String lower = imagePath;
  lower.toLowerCase();
  const char* const* coverExt = std::find_if(std::begin(kCoverExtensions), std::end(kCoverExtensions),
                                             [&lower](const char* ext) { return lower.endsWith(ext); });
  if (coverExt == std::end(kCoverExtensions)) return;
  const std::string base(imagePath.c_str(), imagePath.length() - strlen(*coverExt));

  for (const char* bookExt : kBookExtensions) {
    const std::string bookPath = base + bookExt;
    if (!Storage.exists(bookPath.c_str())) continue;
    const std::string cacheDir = BookRelocation::cacheDirFor(bookPath);
    FsFile dir = Storage.open(cacheDir.c_str());
    if (!dir || !dir.isDirectory()) {
      if (dir) dir.close();
      continue;
    }
    std::vector<std::string> stale;
    char name[64];
    for (FsFile entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
      entry.getName(name, sizeof(name));
      entry.close();
      const size_t len = strlen(name);
      const bool thumb = strncmp(name, "thumb_", 6) == 0 && len > 4 && strcmp(name + len - 4, ".bmp") == 0;
      if (thumb || strcmp(name, "cover.bmp") == 0) stale.emplace_back(cacheDir + "/" + name);
    }
    dir.close();
    for (const std::string& file : stale) Storage.remove(file.c_str());
    LOG_DBG("WEB", "New cover for %s: dropped %u cached cover images", bookPath.c_str(),
            static_cast<unsigned>(stale.size()));
  }
}

void clearBookCacheIfNeeded(const String& filePath) {
  dropThumbnailsForCover(filePath);
  if (FsHelpers::hasEpubExtension(filePath)) {
    Epub(filePath.c_str(), "/.crosspoint").clearCache();
    LOG_DBG("WEB", "Cleared epub cache for: %s", filePath.c_str());
  } else if (FsHelpers::hasXtcExtension(filePath)) {
    Xtc(filePath.c_str(), "/.crosspoint").clearCache();
    LOG_DBG("WEB", "Cleared xtc cache for: %s", filePath.c_str());
  } else if (FsHelpers::hasTxtExtension(filePath) || FsHelpers::hasMarkdownExtension(filePath)) {
    const Txt txt(filePath.c_str(), "/.crosspoint");
    const String cachePath = txt.getCachePath().c_str();
    if (Storage.exists(cachePath.c_str())) {
      Storage.removeDir(cachePath.c_str());
      LOG_DBG("WEB", "Cleared txt cache for: %s", filePath.c_str());
    }
  }
}

// Recursively clear book caches for all ebooks inside a directory
void clearBookCachesInDirectory(const String& dirPath) {
  HalSystem::feedWatchdog();
  yield();
  FsFile dir = Storage.open(dirPath.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }
  char name[500];
  FsFile entry = dir.openNextFile();
  while (entry) {
    HalSystem::feedWatchdog();
    yield();
    entry.getName(name, sizeof(name));
    String childPath = dirPath;
    if (!childPath.endsWith("/")) childPath += "/";
    childPath += name;
    if (entry.isDirectory()) {
      entry.close();
      clearBookCachesInDirectory(childPath);
    } else {
      entry.close();
      clearBookCacheIfNeeded(childPath);
    }
    entry = dir.openNextFile();
  }
  dir.close();
}

String normalizeWebPath(const String& inputPath) {
  if (inputPath.isEmpty() || inputPath == "/") {
    return "/";
  }
  std::string normalized = FsHelpers::normalisePath(inputPath.c_str());
  String result = normalized.c_str();
  if (result.isEmpty()) {
    return "/";
  }
  if (!result.startsWith("/")) {
    result = "/" + result;
  }
  if (result.length() > 1 && result.endsWith("/")) {
    result = result.substring(0, result.length() - 1);
  }
  return result;
}

// For a client-supplied NAME (a rename target), judged as SdFat will store it. A name
// that trims to nothing ("..", "...") is refused too: there is nothing to store.
// Paths go through isProtectedWebPath() instead, which checks every segment.
bool isProtectedItemName(const String& name) {
  const std::string_view stored = ProtectedPaths::effectiveName(std::string_view(name.c_str(), name.length()));
  return stored.empty() || ProtectedPaths::isProtectedName(stored);
}

// True when `s` is usable as a single path component: no separator, and not a
// self/parent reference, so it cannot escape the folder it is joined to.
// Rejecting "/" outright also means plugin folders are flat - no subdirectories.
//
// Only the EXACT components "." and ".." are refused. Refusing every name that
// merely CONTAINED ".." -- what this did before -- also threw out legitimate
// files like "volume..2.epub" or "notes...txt", none of which can traverse
// anywhere: a component with no separator in it resolves inside its parent
// whatever it is called. Narrowed per crosspoint-reader PR #3353.
bool isSafePathComponent(const String& s) {
  return !s.isEmpty() && s.indexOf('/') < 0 && s.indexOf('\\') < 0 && s != "." && s != "..";
}

const char* pluginContentType(const String& file) {
  if (file.endsWith(".js")) return "application/javascript";
  if (file.endsWith(".css")) return "text/css";
  if (file.endsWith(".html")) return "text/html";
  if (file.endsWith(".json")) return "application/json";
  if (file.endsWith(".svg")) return "image/svg+xml";
  return "application/octet-stream";
}

// A manifest carries a title and a mount point. The cap keeps a stray large
// file in a plugin folder from being read into the heap; 4KB is far more than
// the handful of fields the format defines.
constexpr size_t PLUGIN_MANIFEST_MAX_BYTES = 4096;

// /api/plugin-fs is the small-file door; anything bigger belongs in /upload,
// which streams instead of buffering the whole body in the request.
constexpr size_t PLUGIN_FS_MAX_BYTES = 64u * 1024u;

// Host of a URL: between "://" and the next "/", minus any port, and minus any
// userinfo. Dropping everything before an "@" matters - "https://ok.com@evil.com/"
// connects to evil.com, so that is the host the allowlist must judge.
std::string urlHost(const std::string& url) {
  const size_t schemeEnd = url.find("://");
  if (schemeEnd == std::string::npos) return {};
  const size_t start = schemeEnd + 3;
  size_t end = url.find('/', start);
  if (end == std::string::npos) end = url.size();

  std::string host = url.substr(start, end - start);
  const size_t at = host.rfind('@');
  if (at != std::string::npos) host = host.substr(at + 1);
  const size_t colon = host.find(':');
  if (colon != std::string::npos) host.resize(colon);
  return host;
}

// True when the URL's host appears in the calling plugin's manifest
// "allowedHosts". A bare entry must match exactly; one starting with a dot
// matches that suffix (".example.org" allows "covers.example.org"). Absent or
// empty list means nothing is allowed - the relay is opt-in per plugin, and a
// plugin's reach is readable in its own folder before it is installed.
bool relayHostAllowed(const String& plugin, const std::string& url);

// The redirect check for the relay and fetch-to-SD: a redirect is followed only to a host the
// plugin's manifest allows as well, so the allowlist decides where the device actually connects
// and not merely where the first request went.
HttpDownloader::RedirectFilter redirectsWithinAllowlist(const String& plugin) {
  return [plugin](const std::string& next) {
    if (relayHostAllowed(plugin, next)) return true;
    LOG_DBG("WEB", "Redirect refused for %s: %s not in allowedHosts", plugin.c_str(), urlHost(next).c_str());
    return false;
  };
}

// A host matches an allowlist entry exactly, or ends with one that starts with a dot.
bool hostMatches(const std::string& host, const std::string& pattern) {
  if (pattern.empty()) return false;
  if (pattern[0] == '.') {
    return host.size() > pattern.size() && host.compare(host.size() - pattern.size(), pattern.size(), pattern) == 0;
  }
  return host == pattern;
}

// The File Manager's Details dialog is part of the web interface, not a plugin, but its cover search
// reads Goodreads and Google Books covers through the relay like the plugin it grew from. Its scope
// carries a ':', which no FAT folder name can, so no plugin can claim this list.
constexpr char DETAILS_RELAY_SCOPE[] = ":details";
constexpr const char* DETAILS_RELAY_HOSTS[] = {"www.goodreads.com", ".gr-assets.com", "books.google.com"};

bool relayHostAllowed(const String& plugin, const std::string& url) {
  const std::string host = urlHost(url);
  if (host.empty()) return false;
  if (plugin == DETAILS_RELAY_SCOPE) {
    return std::any_of(std::begin(DETAILS_RELAY_HOSTS), std::end(DETAILS_RELAY_HOSTS),
                       [&host](const char* pattern) { return hostMatches(host, pattern); });
  }
  if (!isSafePathComponent(plugin)) return false;

  const std::string dir = PluginLocations::findPluginDir(plugin.c_str());
  if (dir.empty()) return false;

  std::string manifest;
  if (!Storage.readFileToString("WEB", dir + "/manifest.json", PLUGIN_MANIFEST_MAX_BYTES, manifest)) return false;

  JsonDocument m;
  if (deserializeJson(m, manifest) != DeserializationError::Ok || !m["allowedHosts"].is<JsonArray>()) return false;

  for (JsonVariant entry : m["allowedHosts"].as<JsonArray>()) {
    const char* pattern = entry.as<const char*>();
    if (pattern && hostMatches(host, pattern)) return true;
  }
  return false;
}

// Where a plugin may write, or "" when the path is refused.
//
// Containment comes from normalizeWebPath: FsHelpers::normalisePath resolves
// every ".." against the components before it and no-ops once nothing is left,
// so the result cannot climb above the card root. No separate ".." check is
// needed - and a textual one would be wrong, because by this point ".." can
// only survive inside a filename ("my..book.epub"), which is legitimate.
//
// The protection rule is the one /upload applies to its target: every segment,
// not just the file name, which on its own let a plugin write into /.crosspoint.
String pluginWriteTarget(const String& rawPath) {
  String path = normalizeWebPath(rawPath);
  if (path.isEmpty() || path == "/" || path.endsWith("/")) return "";
  if (isProtectedWebPath(path, Target::Item, hiddenAllowed())) return "";
  return path;
}

// Ceiling on a relayed response. Nothing is buffered (see handleRelay), so this
// only bounds how long one request may occupy the single-connection server.
constexpr size_t MAX_RELAY_BYTES = 4u * 1024u * 1024u;

}  // namespace

CrossPointWebServer::CrossPointWebServer() {}

CrossPointWebServer::~CrossPointWebServer() { stop(); }

void CrossPointWebServer::begin() {
  if (running) {
    LOG_DBG("WEB", "Web server already running");
    return;
  }

  // Check if we have a valid network connection (either STA connected or AP mode)
  const wifi_mode_t wifiMode = WiFi.getMode();
  const bool isStaConnected = (wifiMode & WIFI_MODE_STA) && (WiFi.status() == WL_CONNECTED);
  const bool isInApMode = (wifiMode & WIFI_MODE_AP) && (WiFi.softAPgetStationNum() >= 0);  // AP is running

  if (!isStaConnected && !isInApMode) {
    LOG_DBG("WEB", "Cannot start webserver - no valid network (mode=%d, status=%d)", wifiMode, WiFi.status());
    return;
  }

  // Store AP mode flag for later use (e.g., in handleStatus)
  apMode = isInApMode;
  WebAssets::forget();

  LOG_DBG("WEB", "[MEM] Free heap before begin: %d bytes", ESP.getFreeHeap());
  LOG_DBG("WEB", "Network mode: %s", apMode ? "AP" : "STA");

  LOG_DBG("WEB", "Creating web server on port %d...", port);
  // Nothrow: the build is -fno-exceptions, so a bare `new` would abort() on a
  // fragmented heap instead of letting the check below report the failure.
  server = makeUniqueNoThrow<WebServer>(port);
  if (!server) {
    LOG_ERR("WEB", "Failed to create WebServer!");
    return;
  }

  // Disable WiFi sleep to improve responsiveness and prevent 'unreachable' errors.
  // This is critical for reliable web server operation on ESP32.
  WiFi.setSleep(false);

  // Note: WebServer class doesn't have setNoDelay() in the standard ESP32 library.
  // We rely on disabling WiFi sleep for responsiveness.

  LOG_DBG("WEB", "[MEM] Free heap after WebServer allocation: %d bytes", ESP.getFreeHeap());

  // Setup routes
  LOG_DBG("WEB", "Setting up routes...");
  server->on("/", HTTP_GET, [this] { handleWelcomePage(); });
  // The pages come from the SD card (WebAssets.h); this one, in flash, installs them.
  server->on("/setup", HTTP_GET, [this] { handleWebSetupPage(); });
  server->on("/api/web/status", HTTP_GET, [this] { handleWebStatus(); });
  server->on("/api/web/install", HTTP_POST, [this] { handleWebInstall(); });
  server->on("/systeminfo", HTTP_GET, [this] { handleSystemInfoPage(); });
  server->on("/diagnostics", HTTP_GET, [this] { handleDiagnosticsPage(); });
  server->on("/files", HTTP_GET, [this] { handleRoot(); });
  // The bundle's scripts by name (/js/jszip.min.js, /js/metadata-editor.js, ...), so a new one
  // needs no route of its own.
  server->on(UriBraces("/js/{}"), HTTP_GET, [this] { handleBundledScript(); });

  server->on("/api/status", HTTP_GET, [this] { handleStatus(); });
  server->on("/api/status/fast", HTTP_GET, [this] { handleStatusFast(); });
  server->on("/api/diagnostics/boot", HTTP_GET, [this] { handleBootDiagnostics(); });
  server->on("/api/files", HTTP_GET, [this] { handleFileListData(); });
  server->on("/download", HTTP_GET, [this] { handleDownload(); });

  // Upload endpoint with special handling for multipart form data
  server->on("/upload", HTTP_POST, [this] { handleUploadPost(upload); }, [this] { handleUpload(upload); });

  // Create folder endpoint
  server->on("/mkdir", HTTP_POST, [this] { handleCreateFolder(); });

  // Rename file endpoint
  server->on("/rename", HTTP_POST, [this] { handleRename(); });

  // Move file endpoint
  server->on("/move", HTTP_POST, [this] { handleMove(); });

  // Delete file/folder endpoint
  server->on("/delete", HTTP_POST, [this] { handleDelete(); });

  // Settings endpoints
  server->on("/settings", HTTP_GET, [this] { handleSettingsPage(); });
  server->on("/api/settings", HTTP_GET, [this] { handleGetSettings(); });
  server->on("/api/settings", HTTP_POST, [this] { handlePostSettings(); });

  // Font management endpoints
  server->on("/stats", HTTP_GET, [this] { handleStatsPage(); });
  server->on("/api/stats", HTTP_GET, [this] { handleStatsApi(); });
  server->on("/api/stats/export", HTTP_GET, [this] { handleStatsExport(); });
  server->on("/api/stats/remove", HTTP_POST, [this] { handleStatsRemove(); });

  server->on("/fonts", HTTP_GET, [this] { handleFontsPage(); });
  server->on("/api/fonts", HTTP_GET, [this] { handleFontList(); });
  server->on("/api/fonts/manifest", HTTP_GET, [this] { handleFontManifest(); });
  server->on("/api/fonts/download", HTTP_POST, [this] { handleFontDownload(); });
  server->on("/api/fonts/upload", HTTP_POST, [this] { handleFontUpload(); }, [this] { handleFontUploadData(); });
  server->on("/api/fonts/delete", HTTP_POST, [this] { handleFontDelete(); });

  // Singular, so it cannot be mistaken for the /dictionaries folder on the card.
  server->on("/dictionary", HTTP_GET, [this] { handleDictionaryPage(); });

  // OPDS server endpoints
  server->on("/api/opds", HTTP_GET, [this] { handleGetOpdsServers(); });
  server->on("/api/opds", HTTP_POST, [this] { handlePostOpdsServer(); });
  server->on("/api/opds/delete", HTTP_POST, [this] { handleDeleteOpdsServer(); });

  // Wi-Fi credential endpoints
  // Web-UI plugins (SD card)
  server->on("/api/plugins", HTTP_GET, [this] { handlePluginList(); });
  server->on("/plugin", HTTP_GET, [this] { handlePluginFile(); });
  server->on("/api/relay", HTTP_GET, [this] { handleRelay(); });
  server->on("/api/fetch", HTTP_POST, [this] { handleFetchToSd(); });
  server->on("/api/plugin-fs", HTTP_POST, [this] { handlePluginFs(); }, [this] { handlePluginFsData(); });
  server->on("/plugins", HTTP_GET, [this] { handlePluginsPage(); });
  server->on("/api/book/opf", HTTP_GET, [this] { handleBookOpf(); });
  server->on("/api/book/replace", HTTP_POST, [this] { handleBookReplace(); }, [this] { handleBookReplaceData(); });
  server->on("/api/plugins/begin", HTTP_POST, [this] { handlePluginInstallBegin(); });
  server->on(
      "/api/plugins/file", HTTP_POST, [this] { handlePluginInstallFile(); }, [this] { handlePluginInstallFileData(); });
  server->on("/api/plugins/commit", HTTP_POST, [this] { handlePluginInstallCommit(); });
  server->on("/api/plugins/remove", HTTP_POST, [this] { handlePluginRemove(); });
  server->on("/api/plugins/enable", HTTP_POST, [this] { handlePluginEnable(); });

  server->on("/api/wifi", HTTP_GET, [this] { handleGetWifiNetworks(); });
  server->on("/api/wifi", HTTP_POST, [this] { handlePostWifiNetwork(); });
  server->on("/api/wifi/delete", HTTP_POST, [this] { handleDeleteWifiNetwork(); });

  server->onNotFound([this] { handleNotFound(); });
  LOG_DBG("WEB", "[MEM] Free heap after route setup: %d bytes", ESP.getFreeHeap());

  // Collect the WebDAV headers, If-None-Match for the bundle's ETags (WebAssets::serve()) and
  // Referer for the upload log, then register the handler. collectHeaders() replaces the list, so
  // it is called once.
  const char* collectedHeaders[] = {"Depth",      "Destination", "Overwrite",     "If",
                                    "Lock-Token", "Timeout",     "If-None-Match", "Referer"};
  server->collectHeaders(collectedHeaders, sizeof(collectedHeaders) / sizeof(collectedHeaders[0]));
  // Ownership passes to the WebServer, which deletes the handler when stopped.
  // On OOM the HTTP server still comes up; only WebDAV is unavailable.
  if (auto davHandler = makeUniqueNoThrow<WebDAVHandler>()) {
    server->addHandler(davHandler.release());
    LOG_DBG("WEB", "WebDAV handler initialized");
  } else {
    LOG_ERR("WEB", "OOM: WebDAV handler — WebDAV disabled");
  }

  server->begin();

  // Start WebSocket server for fast binary uploads. On OOM the rest of the
  // server stays up; uploads fall back to the plain HTTP path. Every other
  // wsServer use is already null-guarded.
  LOG_DBG("WEB", "Starting WebSocket server on port %d...", wsPort);
  wsServer = makeUniqueNoThrow<WebSocketsServer>(wsPort);
  if (wsServer) {
    wsInstance = const_cast<CrossPointWebServer*>(this);
    wsServer->begin();
    wsServer->onEvent(wsEventCallback);
    LOG_DBG("WEB", "WebSocket server started");
  } else {
    LOG_ERR("WEB", "OOM: WebSocket server — binary uploads disabled");
  }

  udpActive = udp.begin(LOCAL_UDP_PORT);
  LOG_DBG("WEB", "Discovery UDP %s on port %d", udpActive ? "enabled" : "failed", LOCAL_UDP_PORT);

  running = true;

  LOG_DBG("WEB", "Web server started on port %d", port);
  // Show the correct IP based on network mode
  const String ipAddr = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  LOG_DBG("WEB", "Access at http://%s/", ipAddr.c_str());
  LOG_DBG("WEB", "WebSocket at ws://%s:%d/", ipAddr.c_str(), wsPort);
  LOG_DBG("WEB", "[MEM] Free heap after server.begin(): %d bytes", ESP.getFreeHeap());
}

// Ported from crosspoint-reader PR #3732 ("fix: let Back leave File Transfer during a slow HTTP
// upload", Tuan Q. Nguyen / @martinqnguyen). The hook, the per-chunk drop and the END guard below
// are theirs; what the activity checks inside the hook differs (see startWebServer()).
bool CrossPointWebServer::dropUploadIfCancelled() const {
  if (!uploadCancelCheck || !uploadCancelCheck()) return false;
  // Closing the socket makes WebServer's next body read fail. It then raises UPLOAD_FILE_ABORTED,
  // whose handler deletes the partial file, and handleClient() returns with the client released.
  LOG_DBG("WEB", "[UPLOAD] Cancelled on the device, dropping the client");
  server->client().stop();
  return true;
}

void CrossPointWebServer::abortWsUpload(const char* tag) {
  wsUploadFile.close();
  String filePath = wsUploadPath;
  if (!filePath.endsWith("/")) filePath += "/";
  filePath += wsUploadFileName;
  if (Storage.remove(filePath.c_str())) {
    LOG_DBG(tag, "Deleted incomplete upload: %s", filePath.c_str());
  } else {
    LOG_DBG(tag, "Failed to delete incomplete upload: %s", filePath.c_str());
  }
  wsUploadInProgress = false;
  wsUploadClientNum = 255;
  wsLastProgressSent = 0;
}

void CrossPointWebServer::stop() {
  if (!running || !server) {
    LOG_DBG("WEB", "stop() called but already stopped (running=%d, server=%p)", running, server.get());
    return;
  }

  LOG_DBG("WEB", "STOP INITIATED - setting running=false first");
  running = false;  // Set this FIRST to prevent handleClient from using server

  LOG_DBG("WEB", "[MEM] Free heap before stop: %d bytes", ESP.getFreeHeap());

  // Close any in-progress WebSocket upload and remove partial file
  if (wsUploadInProgress && wsUploadFile) {
    abortWsUpload("WEB");
  }

  // Stop WebSocket server
  if (wsServer) {
    LOG_DBG("WEB", "Stopping WebSocket server...");
    wsServer->close();
    wsServer.reset();
    wsInstance = nullptr;
    LOG_DBG("WEB", "WebSocket server stopped");
  }

  if (udpActive) {
    udp.stop();
    udpActive = false;
  }

  // Brief delay to allow any in-flight handleClient() calls to complete
  delay(20);

  server->stop();
  LOG_DBG("WEB", "[MEM] Free heap after server->stop(): %d bytes", ESP.getFreeHeap());

  // Brief delay before deletion
  delay(10);

  server.reset();
  LOG_DBG("WEB", "Web server stopped and deleted");
  LOG_DBG("WEB", "[MEM] Free heap after delete server: %d bytes", ESP.getFreeHeap());

  // Note: Static upload variables (uploadFileName, uploadPath, uploadError) are declared
  // later in the file and will be cleared when they go out of scope or on next upload
  LOG_DBG("WEB", "[MEM] Free heap final: %d bytes", ESP.getFreeHeap());
}

void CrossPointWebServer::handleClient() {
  static unsigned long lastDebugPrint = 0;
  static unsigned long lastSlowPoll = 0;

  // Check running flag FIRST before accessing server
  if (!running) {
    return;
  }

  // Double-check server pointer is valid
  if (!server) {
    LOG_DBG("WEB", "WARNING: handleClient called with null server!");
    return;
  }

  // Print debug every 10 seconds to confirm handleClient is being called
  if (millis() - lastDebugPrint > 10000) {
    LOG_DBG("WEB", "handleClient active, server running on port %d", port);
    lastDebugPrint = millis();
  }

  server->handleClient();

  // WebSocket loop and UDP discovery are polled once per ~10ms, not on every
  // handleClient() call. The activity loop calls handleClient() up to 500 times
  // per outer iteration; calling wsServer->loop() every time causes excessive
  // heap churn from String allocations in the WebSocket library.
  const unsigned long now = millis();
  if (now - lastSlowPoll < 10) {
    return;
  }
  lastSlowPoll = now;

  if (wsServer) {
    wsServer->loop();
  }

  // Respond to discovery broadcasts
  if (udpActive) {
    int packetSize = udp.parsePacket();
    if (packetSize > 0) {
      char buffer[16];
      int len = udp.read(buffer, sizeof(buffer) - 1);
      if (len > 0) {
        buffer[len] = '\0';
        if (strcmp(buffer, "hello") == 0) {
          String hostname = WiFi.getHostname();
          if (hostname.isEmpty()) {
            hostname = "crosspoint";
          }
          String message = "crosspoint (on " + hostname + ");" + String(wsPort);
          udp.beginPacket(udp.remoteIP(), udp.remotePort());
          udp.write(reinterpret_cast<const uint8_t*>(message.c_str()), message.length());
          udp.endPacket();
        }
      }
    }
  }
}

CrossPointWebServer::WsUploadStatus CrossPointWebServer::getWsUploadStatus() const {
  WsUploadStatus status;
  status.inProgress = wsUploadInProgress;
  status.received = wsUploadReceived;
  status.total = wsUploadSize;
  status.filename = wsUploadFileName.c_str();
  status.lastCompleteName = wsLastCompleteName.c_str();
  status.lastCompleteSize = wsLastCompleteSize;
  status.lastCompleteAt = wsLastCompleteAt;
  status.lastDeleteName = lastDeleteName.c_str();
  status.lastDeleteCount = lastDeleteCount;
  status.lastDeleteAt = lastDeleteAt;
  return status;
}

// The setup page is never cached: it answers at "/" only until the web interface
// is installed, and a cached copy would keep showing there afterwards.
static void sendSetupPage(WebServer* server) {
  server->sendHeader("Content-Encoding", "gzip");
  server->sendHeader("Cache-Control", "no-store");
  server->send_P(200, "text/html", WebSetupPageHtml, sizeof(WebSetupPageHtml));
}

// Serialize a JsonDocument to the wire with no String allocation.
// Uses measureJson to get exact payload size, then malloc+serializeJson+sendContent+free.
// WDT is reset before and after sendContent so the write can't outlast the watchdog.
// On malloc failure, sends a 500.
static void sendJson(WebServer* server, int code, const JsonDocument& doc) {
  const size_t payloadSize = measureJson(doc);
  char* buf = static_cast<char*>(malloc(payloadSize + 1));
  if (!buf) {
    LOG_DBG("WEB", "sendJson: malloc(%zu) failed", payloadSize + 1);
    server->send(500, "application/json", "{\"error\":\"OOM\"}");
    return;
  }
  serializeJson(doc, buf, payloadSize + 1);
  server->setContentLength(payloadSize);
  server->send(code, "application/json", "");
  HalSystem::feedWatchdog();
  server->sendContent(buf, payloadSize);
  free(buf);
  HalSystem::feedWatchdog();
}

// A page from the SD card bundle, or a redirect to the setup page while the
// bundle is not installed.
void CrossPointWebServer::serveBundlePage(const web_bundle::Index page) const {
  // Streaming claims a 4KB chunk buffer, same as /download.
  if (rejectIfLowMemory(server.get(), MIN_HEAP_FOR_HTML)) return;
  const int32_t t0 = millis();
  if (!WebAssets::serve(*server, page)) {
    server->sendHeader("Location", "/setup");
    server->send(302, "text/plain", "");
    return;
  }
  LOG_DBG("WEB", "Served %s in %d ms", web_bundle::ENTRIES[page].file, static_cast<int>(millis() - t0));
}

void CrossPointWebServer::handleRoot() const {
  LOG_WEB_MEM("root_enter");
  serveBundlePage(web_bundle::FilesPageHtml);
  LOG_WEB_MEM("root_exit");
}

// "/" is the welcome page once the bundle is installed and the setup page until
// then. The setup page is never stored and the bundle's pages are revalidated
// (WebAssets::serve()), so a browser does not keep showing the one it saw last.
void CrossPointWebServer::handleWelcomePage() const {
  if (rejectIfLowMemory(server.get(), MIN_HEAP_FOR_HTML)) return;
  if (WebAssets::serve(*server, web_bundle::WelcomePageHtml)) return;
  sendSetupPage(server.get());
}

void CrossPointWebServer::handleWebSetupPage() const {
  if (rejectIfLowMemory(server.get(), MIN_HEAP_FOR_HTML)) return;
  sendSetupPage(server.get());
}

void CrossPointWebServer::handleWebStatus() const {
  if (rejectIfLowMemory(server.get())) return;
  JsonDocument doc;
  doc["state"] = WebAssets::stateName(WebAssets::state());
  doc["release"] = WebAssets::releaseTag();
  doc["repo"] = WebAssets::repository();
  doc["asset"] = WebAssets::assetName();
  doc["online"] = !apMode;
  doc["onCard"] = WebAssets::bundleOnCard();
  if (!apMode) doc["rssi"] = WiFi.RSSI();
  // An upload that broke off: the setup page's browser saw only the connection go.
  if (const UploadState::Failure& f = upload.failure; f.set) {
    JsonObject last = doc["lastUpload"].to<JsonObject>();
    last["name"] = f.name;
    last["received"] = f.received;
    last["expected"] = f.expected;
    last["ms"] = f.ms;
    last["idleMs"] = f.idleMs;
    last["reason"] = f.reason;
    if (!apMode) last["rssi"] = f.rssi;
  }
  sendJson(server.get(), 200, doc);
}

// POST /api/web/install?source=release|card
void CrossPointWebServer::handleWebInstall() const {
  if (rejectIfLowMemory(server.get())) return;
  const String source = server->arg("source");
  WebAssets::InstallError err;
  if (source == "card") {
    err = WebAssets::installFromCard();
  } else if (source == "release") {
    if (apMode) {
      server->send(409, "application/json", "{\"ok\":false,\"error\":\"no internet connection in hotspot mode\"}");
      return;
    }
    err = WebAssets::installFromRelease();
  } else {
    server->send(400, "application/json", "{\"ok\":false,\"error\":\"unknown source\"}");
    return;
  }

  JsonDocument doc;
  doc["ok"] = (err == WebAssets::InstallError::None);
  if (err != WebAssets::InstallError::None) doc["error"] = WebAssets::errorText(err);
  sendJson(server.get(), err == WebAssets::InstallError::None ? 200 : 500, doc);
}

void CrossPointWebServer::handleSystemInfoPage() const { serveBundlePage(web_bundle::HomePageHtml); }

void CrossPointWebServer::handleDiagnosticsPage() const { serveBundlePage(web_bundle::DiagnosticsPageHtml); }

void CrossPointWebServer::handleStatsPage() const { serveBundlePage(web_bundle::StatsPageHtml); }

void CrossPointWebServer::handleStatsApi() const {
  if (rejectIfLowMemory(server.get())) return;
  LOG_WEB_MEM("stats_api_enter");
  // The figures the on-device screens show, with streaks and todayDayIndex pre-computed so the
  // browser doesn't have to recreate the day-index math; the day arrays go across untouched so it
  // can render the sparkline.
  //
  // Generated from the history file one book at a time, never loaded whole: the meta and one slot,
  // whatever the history holds.
  bool empty = false;
  {
    ReadingStatsStore::Summary summary;
    if (READING_STATS.querySummary(summary) != ReadingStatsStore::ReadResult::Ok) {
      server->send(500, "application/json", "{\"error\":\"Reading stats could not be read\"}");
      return;
    }
    empty = summary.bookCount == 0 && summary.totalSeconds == 0;
  }  // its day buckets go before the payload's own are read
  if (empty) {
    server->send(200, "application/json", "{\"totalSeconds\":0,\"books\":[]}");  // no history yet
    return;
  }
  ChunkedResponse response(server.get());
  ChunkedPrint out(response);
  // The 200 goes out only once the history is open and its buffers are allocated, so a failure
  // still reaches the browser as one.
  const auto result = READING_STATS.writeDashboard(out, currentLocalDayIndex(), [this] {
    server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    server->send(200, "application/json", "");
  });
  if (result != ReadingStatsStore::ReadResult::Ok) {
    server->send(500, "application/json", "{\"error\":\"Reading stats could not be read\"}");
    return;
  }
  response.finish();
  LOG_WEB_MEM("stats_api_exit");
}

// Body: {"docId": "..."}. Same removal as the device's per-book stats screen: the book's entry goes
// and its time comes back out of the totals: the store frees its directory entry and rewrites the
// global figures, copy-on-write.
void CrossPointWebServer::handleStatsRemove() const {
  if (rejectIfLowMemory(server.get())) return;
  JsonDocument req;
  if (deserializeJson(req, server->arg("plain")) || !req["docId"].is<const char*>()) {
    server->send(400, "application/json", "{\"error\":\"Invalid request\"}");
    return;
  }
  const std::string docId = req["docId"].as<const char*>();

  switch (READING_STATS.removeBook(docId)) {
    case ReadingStatsStore::WriteResult::Done:
      LOG_DBG("WEB", "Removed from reading stats: %s", docId.c_str());
      server->send(200, "application/json", "{\"ok\":true}");
      return;
    case ReadingStatsStore::WriteResult::NotFound:
      server->send(404, "application/json", "{\"error\":\"Book not found\"}");
      return;
    case ReadingStatsStore::WriteResult::NoMemory:
    case ReadingStatsStore::WriteResult::Failed:
      server->send(500, "application/json", "{\"error\":\"Could not update the reading stats\"}");
      return;
  }
}

void CrossPointWebServer::handleStatsExport() const {
  if (rejectIfLowMemory(server.get())) return;
  // The reading-stats.json format older firmware reads, generated from the history file: the
  // backup, and the way back after a downgrade.
  bool empty = false;
  {
    ReadingStatsStore::Summary summary;
    if (READING_STATS.querySummary(summary) != ReadingStatsStore::ReadResult::Ok) {
      server->send(500, "application/json", "{}");
      return;
    }
    empty = summary.bookCount == 0 && summary.totalSeconds == 0;
  }
  if (empty) {
    server->send(404, "application/json", "{}");
    return;
  }
  ChunkedResponse response(server.get());
  ChunkedPrint out(response);
  // As for the dashboard: the headers only once nothing can fail, or the browser would save a
  // truncated backup as if it were whole.
  const auto result = READING_STATS.writeExport(out, [this] {
    server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    server->sendHeader("Content-Disposition", "attachment; filename=\"reading-stats.json\"");
    server->send(200, "application/json", "");
  });
  if (result != ReadingStatsStore::ReadResult::Ok) {
    server->send(500, "application/json", "{}");
    return;
  }
  response.finish();
}

void CrossPointWebServer::handleBundledScript() const {
  if (rejectIfLowMemory(server.get(), MIN_HEAP_FOR_HTML)) return;
  // A script, so no redirect to a page: each page copes without one (the File Manager says, for
  // instance, that Details is not available).
  if (!WebAssets::serveScript(*server, server->pathArg(0).c_str())) server->send(404, "text/plain", "Not installed");
}

void CrossPointWebServer::handleNotFound() const {
  String message = "404 Not Found\n\n";
  message += "URI: " + server->uri() + "\n";
  server->send(404, "text/plain", message);
}

void CrossPointWebServer::handleStatus() const {
  // SD stats require a freeClusterCount() scan of the whole FAT, which can
  // block the (single-threaded) server for tens of seconds on a large card.
  // Only collect them when explicitly asked (?phase=full): third-party
  // clients like the Calibre plugin poll plain /api/status with short
  // timeouts right before listing files, and a stalled server makes those
  // listings time out (books on device never show up in Calibre).
  const bool fullSd = server->hasArg("phase") && server->arg("phase").equalsIgnoreCase("full");
  if (rejectIfLowMemory(server.get())) return;
  LOG_WEB_MEM("status_enter");
  LOG_DBG("SYSINFO", "handleStatus request received (fullSd=%d)", fullSd);
  int32_t t0 = millis();
  SystemStatus status = SystemStatus::collectFast();
  int32_t t1 = millis();
  LOG_WEB_MEM("status_after_collectFast");
  LOG_DBG("SYSINFO", "Collected fast status in %d ms (fullSd=%d)", t1 - t0, fullSd);
  if (fullSd) {
    LOG_DBG("SYSINFO", "handleStatus will collect SD stats");
    SystemStatus::fillSdStatus(status);
    LOG_WEB_MEM("status_after_fillSd");
  }

  JsonDocument doc;
  doc["version"] = status.version;
  doc["deviceType"] = status.deviceType;
  // Upstream-compatible alias used by the Calibre plugin's model detection.
  doc["device"] = status.deviceType;
  doc["displayWidth"] = status.displayWidth;
  doc["displayHeight"] = status.displayHeight;
  doc["chipVersion"] = status.chipVersion;
  doc["cpuMHz"] = status.cpuFreqMHz;
  doc["ip"] = status.ip;
  doc["mode"] = status.wifiMode;
  doc["rssi"] = status.rssi;
  doc["macAddress"] = status.macAddress;
  doc["freeHeap"] = status.freeHeapBytes;
  doc["minFreeHeap"] = status.minFreeHeapBytes;
  doc["maxAllocHeap"] = status.maxAllocHeapBytes;
  doc["flashTotal"] = status.flashBytes;
  doc["appPartitionSize"] = status.flashAppPartitionSize;
  doc["batteryPercent"] = status.batteryPercent;
  doc["charging"] = status.charging;
  doc["uptime"] = status.uptimeSeconds;
  doc["sdReady"] = fullSd;
  doc["sdTotal"] = status.sdTotalBytes;
  doc["sdUsed"] = status.sdUsedBytes;
  doc["sdFree"] = status.sdFreeBytes;

  LOG_WEB_MEM("status_exit");
  sendJson(server.get(), 200, doc);
}

void CrossPointWebServer::handleStatusFast() const {
  if (rejectIfLowMemory(server.get())) return;
  LOG_DBG("SYSINFO", "handleStatusFast request received");
  int32_t t0 = millis();
  SystemStatus status = SystemStatus::collectFast();
  int32_t t1 = millis();
  LOG_DBG("SYSINFO", "Collected fast-only status in %d ms", t1 - t0);

  JsonDocument doc;
  doc["version"] = status.version;
  doc["deviceType"] = status.deviceType;
  // Upstream-compatible alias used by the Calibre plugin's model detection.
  doc["device"] = status.deviceType;
  doc["displayWidth"] = status.displayWidth;
  doc["displayHeight"] = status.displayHeight;
  doc["chipVersion"] = status.chipVersion;
  doc["cpuMHz"] = status.cpuFreqMHz;
  doc["ip"] = status.ip;
  doc["mode"] = status.wifiMode;
  doc["rssi"] = status.rssi;
  doc["macAddress"] = status.macAddress;
  doc["freeHeap"] = status.freeHeapBytes;
  doc["minFreeHeap"] = status.minFreeHeapBytes;
  doc["maxAllocHeap"] = status.maxAllocHeapBytes;
  doc["flashTotal"] = status.flashBytes;
  doc["appPartitionSize"] = status.flashAppPartitionSize;
  doc["batteryPercent"] = status.batteryPercent;
  doc["charging"] = status.charging;
  doc["uptime"] = status.uptimeSeconds;
  doc["sdReady"] = false;
  // Still include SD stats in response, but client should ignore them when sdReady=false
  doc["sdTotal"] = status.sdTotalBytes;
  doc["sdUsed"] = status.sdUsedBytes;
  doc["sdFree"] = status.sdFreeBytes;

  sendJson(server.get(), 200, doc);
}

namespace {

// The pieces of GET /api/diagnostics/boot. Same accessors and same untranslated tokens as
// Settings > System > Boot Diagnostics, so a report saved from the browser reads like a photo
// of that page and two reports stay comparable.
void fillBootHardware(JsonDocument& doc) {
  doc["board"] = BoardConfig::ACTIVE.name;
  doc["displayController"] = displayControllerName(BoardConfig::ACTIVE.displayController);
  const auto& probe = freeink::getXteinkDisplayProbeDiag();
  doc["promotedByProbe"] = probe.promoted;
  if (probe.valid) {
    char buf[48];
    snprintf(buf, sizeof(buf), "ver %02X %02X %02X %02X %02X flg %02X v%u%s", probe.ver[0], probe.ver[1], probe.ver[2],
             probe.ver[3], probe.ver[4], probe.flg, probe.verdict, probe.mtpValid ? " mtp" : "");
    doc["panelProbe"] = buf;
  } else {
    doc["panelProbe"] = nullptr;  // no probe ran: itself a finding
  }
}

void fillThisBoot(JsonDocument& doc, const BootDiag::Record* records, const uint8_t count) {
  doc["resetReason"] = BootDiag::resetReasonName(static_cast<uint8_t>(esp_reset_reason()));
  doc["wakeCause"] = BootDiag::wakeCauseName(static_cast<uint8_t>(esp_sleep_get_wakeup_cause()));
  // BootDiag::previousSessionEndedWithoutSleep() on the records already loaded: two boots in a
  // row mean the session between them never reached the sleep path. With fewer than two records
  // (no card, a fresh ring) nothing is known, and null says so instead of "slept".
  if (count >= 2) {
    doc["previousSessionSlept"] = !(records[0].kind == BootDiag::KindBoot && records[1].kind == BootDiag::KindBoot);
  } else {
    doc["previousSessionSlept"] = nullptr;
  }

  const auto& check = BootDiag::wakeCheck();
  const JsonObject gate = doc["wakeGate"].to<JsonObject>();
  gate["verdict"] = HalGPIO::wakeVerdictName(check.verdict);
  gate["heldMs"] = check.heldMs;
  gate["decidedAtMs"] = check.decidedAtMs;
  gate["requiredMs"] = CrossPointSettings::getPowerWakeHoldDuration();

  doc["firstPaintMs"] = BootDiag::phaseMs(BootDiag::BootPhase::FirstPaint);
  // Absolute stamps of the phases this boot reached; the page turns them into the per-phase
  // costs the device shows. Unreached phases are left out there too.
  const JsonArray phases = doc["phases"].to<JsonArray>();
  for (uint8_t i = 0; i < static_cast<uint8_t>(BootDiag::BootPhase::Count); i++) {
    const auto phase = static_cast<BootDiag::BootPhase>(i);
    if (!BootDiag::phaseReached(phase)) continue;
    const JsonObject entry = phases.add<JsonObject>();
    entry["name"] = BootDiag::phaseName(phase);
    entry["ms"] = BootDiag::phaseMs(phase);
  }

  // The ring's count and the NVS lifetime count side by side, as on the device: a zero in the
  // first with a non-zero second means aborts were counted but lost on the way to the card.
  const BootDiag::AbortCounts counts = BootDiag::abortCounts();
  const JsonObject aborted = doc["abortedBoots"].to<JsonObject>();
  aborted["thisRun"] = BootDiag::abortsInCurrentRun(records, count);
  aborted["lifetime"] = counts.lifetime;
  aborted["undrained"] = counts.undrained;

  const auto& stats = powerManager.lightSleepStats();
  const JsonObject light = doc["lightSleep"].to<JsonObject>();
  light["sleptMs"] = stats.sleptMs;
  light["awakeMs"] = stats.awakeMs;
  light["attempts"] = stats.attempts;
  light["gpioWakes"] = stats.wakeGpio;
}

// One history line. The record's fields mean different things per kind (BootDiagRing.h), so
// each kind gets its own names rather than the raw slots.
void fillBootRecord(JsonDocument& doc, const BootDiag::Record& record) {
  doc["seq"] = record.seq;
  switch (record.kind) {
    case BootDiag::KindSleep:
      doc["kind"] = "sleep";
      doc["outcome"] = BootDiag::outcomeName(BootDiag::outcomeOf(record));
      doc["stage"] = BootDiag::stageName(static_cast<BootDiag::SleepStage>(record.code));
      doc["trigger"] = BootDiag::triggerName(static_cast<BootDiag::SleepTrigger>(record.reason));
      doc["latchKept"] = (record.flags & BootDiag::kFlagKeepClock) != 0;
      doc["fromReader"] = (record.flags & BootDiag::kFlagFromReader) != 0;
      doc["releaseWaitMs"] = record.msA;
      doc["releaseTimedOut"] = (record.flags & BootDiag::kFlagReleaseTimeout) != 0;
      doc["uptimeS"] = record.msB;
      return;
    case BootDiag::KindBoot:
      doc["kind"] = "boot";
      doc["resetReason"] = BootDiag::resetReasonName(record.reason);
      doc["wakeCause"] = BootDiag::wakeCauseName(record.flags);
      doc["gate"] = HalGPIO::wakeVerdictName(static_cast<HalGPIO::WakeVerdict>(record.code));
      doc["gateDecidedMs"] = record.msA;
      doc["gateHeldMs"] = record.msB;
      doc["sdMountMs"] = record.msC;
      return;
    case BootDiag::KindAborted:
      doc["kind"] = "aborted";
      doc["trigger"] = BootDiag::triggerName(static_cast<BootDiag::SleepTrigger>(record.reason));
      doc["gate"] = HalGPIO::wakeVerdictName(static_cast<HalGPIO::WakeVerdict>(record.code));
      doc["count"] = record.msA;
      doc["capped"] = record.msA >= BootDiag::kAbortCountCap;
      return;
    default:
      doc["kind"] = "unknown";
      return;
  }
}

}  // namespace

// GET /api/diagnostics/boot: this boot's start and the persisted sleep/boot history, for the
// Diagnostics page and the report it saves.
void CrossPointWebServer::handleBootDiagnostics() const {
  if (rejectIfLowMemory(server.get())) return;
  // The ring's 256 bytes go on the heap, not the stack: handleClient() runs on the loop task
  // beneath the whole activity call chain (see ChunkedResponse).
  const auto records = makeUniqueNoThrow<BootDiag::Record[]>(BootDiag::kCapacity);
  if (!records) {
    server->send(500, "application/json", "{\"error\":\"OOM\"}");
    return;
  }
  const uint8_t count = BootDiag::loadRecords(records.get(), BootDiag::kCapacity);

  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  ChunkedResponse response(server.get());
  ChunkedPrint out(response);
  // One small document per section and per history line, cleared in between as the file
  // listing does, rather than one holding all sixteen records at once.
  JsonDocument doc;
  out.print("{\"hardware\":");
  fillBootHardware(doc);
  serializeJson(doc, out);

  doc.clear();
  out.print(",\"thisBoot\":");
  fillThisBoot(doc, records.get(), count);
  serializeJson(doc, out);

  out.print(",\"history\":[");
  for (uint8_t i = 0; i < count; i++) {
    doc.clear();
    fillBootRecord(doc, records[i]);
    if (i > 0) out.print(',');
    serializeJson(doc, out);
  }
  out.print("]}");
  response.finish();
}

void CrossPointWebServer::scanFiles(const char* path, const FileVisitor visitor, void* context) const {
  FsFile root = Storage.open(path);
  if (!root) {
    LOG_DBG("WEB", "Failed to open directory: %s", path);
    return;
  }

  if (!root.isDirectory()) {
    LOG_DBG("WEB", "Not a directory: %s", path);
    root.close();
    return;
  }

  LOG_DBG("WEB", "Scanning files in: %s", path);

  FsFile file = root.openNextFile();
  char name[500];
  while (file) {
    file.getName(name, sizeof(name));
    auto fileName = String(name);

    // Dot items only when the user has asked for hidden files; the system folders never.
    // The same rules isProtectedWebPath() enforces, so nothing listed here is refused
    // when clicked, bar the credential stores.
    const std::string_view entryName(fileName.c_str(), fileName.length());
    const bool shouldHide =
        (!hiddenAllowed() && ProtectedPaths::isDotName(entryName)) || ProtectedPaths::isSystemName(entryName);

    if (!shouldHide) {
      FileInfo info;
      info.name = fileName;
      info.isDirectory = file.isDirectory();

      if (info.isDirectory) {
        info.size = 0;
        info.isEpub = false;
      } else {
        info.size = file.size();
        info.isEpub = isEpubFile(info.name);
      }

      visitor(info, context);
    }

    file.close();
    yield();                    // Yield to allow WiFi and other tasks to process during long scans
    HalSystem::feedWatchdog();  // Reset watchdog to prevent timeout on large directories
    file = root.openNextFile();
  }
  root.close();
}

bool CrossPointWebServer::isEpubFile(const String& filename) const { return FsHelpers::hasEpubExtension(filename); }

void CrossPointWebServer::handleFileListData() const {
  if (rejectIfLowMemory(server.get())) return;
  // Get current path from query string (default to root)
  String currentPath = "/";
  if (server->hasArg("path")) {
    // normalizeWebPath, not a bare startsWith("/"): the naive form leaves ".." in
    // the string, and "/books/../.private" would pass a check on its components
    // while the filesystem resolves it into the dot-folder the check exists to
    // protect. Ported from crosspoint-reader PR #3353 (Sylve / @s0lness).
    currentPath = normalizeWebPath(server->arg("path"));
  }
  LOG_DBG("WEB", "File list request for path: %s", currentPath.c_str());
  if (isProtectedWebPath(currentPath, Target::Directory, hiddenAllowed())) {
    server->send(403, "text/plain", "Cannot access protected items");
    return;
  }

  LOG_WEB_MEM("files_enter");
  HalSystem::feedWatchdog();
  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  HalSystem::feedWatchdog();
  ChunkedJsonArray out(server.get());
  char output[512];
  constexpr size_t outputSize = sizeof(output);
  JsonDocument doc;

  struct FileListContext {
    ChunkedJsonArray* out;
    char* output;
    JsonDocument* doc;
  } context{&out, output, &doc};

  scanFiles(
      currentPath.c_str(),
      [](const FileInfo& info, void* rawContext) {
        auto& ctx = *static_cast<FileListContext*>(rawContext);
        HalSystem::feedWatchdog();
        ctx.doc->clear();
        (*ctx.doc)["name"] = info.name;
        (*ctx.doc)["size"] = info.size;
        (*ctx.doc)["isDirectory"] = info.isDirectory;
        (*ctx.doc)["isEpub"] = info.isEpub;

        const size_t written = serializeJson(*ctx.doc, ctx.output, outputSize);
        if (written >= outputSize) {
          LOG_DBG("WEB", "Skipping file entry with oversized JSON for name: %s", info.name.c_str());
          return;
        }
        ctx.out->addEntry(ctx.output, written);
      },
      &context);

  HalSystem::feedWatchdog();
  out.finish();
  LOG_WEB_MEM("files_exit");
  LOG_DBG("WEB", "Served file listing page for path: %s", currentPath.c_str());
}

void CrossPointWebServer::handleDownload() const {
  if (!server->hasArg("path")) {
    server->send(400, "text/plain", "Missing path");
    return;
  }

  // Resolved before the protection check below -- see handleFileListData.
  // crosspoint-reader PR #3353.
  String itemPath = normalizeWebPath(server->arg("path"));
  if (itemPath.isEmpty() || itemPath == "/") {
    server->send(400, "text/plain", "Invalid path");
    return;
  }
  if (isProtectedWebPath(itemPath, Target::Item, hiddenAllowed())) {
    server->send(403, "text/plain", "Cannot access protected items");
    return;
  }

  if (!Storage.exists(itemPath.c_str())) {
    server->send(404, "text/plain", "Item not found");
    return;
  }

  FsFile file = Storage.open(itemPath.c_str());
  if (!file) {
    server->send(500, "text/plain", "Failed to open file");
    return;
  }
  if (file.isDirectory()) {
    file.close();
    server->send(400, "text/plain", "Path is a directory");
    return;
  }

  String contentType = "application/octet-stream";
  if (isEpubFile(itemPath)) {
    contentType = "application/epub+zip";
  }

  char nameBuf[128] = {0};
  String filename = "download";
  if (file.getName(nameBuf, sizeof(nameBuf))) {
    filename = nameBuf;
  }

  server->setContentLength(file.size());
  server->sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  server->send(200, contentType.c_str(), "");

  NetworkClient client = server->client();
  bool downloadOk = HttpFileStreamer::streamFileToClient(file, client);
  client.clear();

  if (!downloadOk) {
    LOG_DBG("WEB", "Download interrupted while streaming: %s", itemPath.c_str());
  }
}

// Diagnostic counters for upload performance analysis
static unsigned long uploadStartTime = 0;
static unsigned long totalWriteTime = 0;
static size_t writeCount = 0;

static bool flushUploadBuffer(CrossPointWebServer::UploadState& state) {
  if (state.bufferPos > 0 && state.file) {
    HalSystem::feedWatchdog();  // Reset watchdog before potentially slow SD write
    const unsigned long writeStart = millis();
    const size_t written = state.file.write(state.buffer.data(), state.bufferPos);
    totalWriteTime += millis() - writeStart;
    writeCount++;
    HalSystem::feedWatchdog();  // Reset watchdog after SD write

    if (written != state.bufferPos) {
      LOG_DBG("WEB", "[UPLOAD] Buffer flush failed: expected %d, wrote %d", state.bufferPos, written);
      state.bufferPos = 0;
      return false;
    }
    state.bufferPos = 0;
  }
  return true;
}

// Closes the upload's file and deletes it. A write that failed part-way leaves a file cut short,
// and the Library and Home would list that truncated book.
// Ported from CrossInk commit e092f81da ("fix: protect files and reduce unnecessary reader work",
// Julia Nguyen): there a failed write removes the partial file; the helper is ours.
static void discardUploadFile(CrossPointWebServer::UploadState& state) {
  state.file.close();
  String filePath = state.path;
  if (!filePath.endsWith("/")) filePath += "/";
  filePath += state.fileName;
  Storage.remove(filePath.c_str());
}

void CrossPointWebServer::handleUpload(UploadState& state) const {
  static size_t lastLoggedSize = 0;
  static unsigned long lastLoggedAt = 0;

  // Reset watchdog at start of every upload callback - HTTP parsing can be slow
  HalSystem::feedWatchdog();

  // Safety check: ensure server is still valid
  if (!running || !server) {
    LOG_DBG("WEB", "[UPLOAD] ERROR: handleUpload called but server not running!");
    return;
  }

  const HTTPUpload& up = server->upload();

  if (up.status == UPLOAD_FILE_START) {
    // Reset watchdog - this is the critical 1% crash point
    HalSystem::feedWatchdog();
    tolerateUploadStalls(*server);

    state.fileName = up.filename;
    state.size = 0;
    state.success = false;
    state.error = "";
    state.expected = server->clientContentLength();
    state.cancelled = false;
    state.failure = {};
    uploadStartTime = millis();
    state.lastDataAt = uploadStartTime;
    lastLoggedSize = 0;
    lastLoggedAt = uploadStartTime;
    state.bufferPos = 0;
    totalWriteTime = 0;
    writeCount = 0;
    state.buffer.resize(UploadState::UPLOAD_BUFFER_SIZE);

    // The client controls this name outright, and it is concatenated onto the
    // target directory below. Without this it can carry its own separators and
    // land the write anywhere on the card. crosspoint-reader PR #3353.
    if (!isSafePathComponent(state.fileName)) {
      state.error = "Invalid file name";
      LOG_DBG("WEB", "[UPLOAD] Rejected unsafe filename: %s", state.fileName.c_str());
      return;
    }

    // Get upload path from query parameter (defaults to root if not specified)
    // Note: We use query parameter instead of form data because multipart form
    // fields aren't available until after file upload completes
    if (server->hasArg("path")) {
      state.path = normalizeWebPath(server->arg("path"));
    } else {
      state.path = "/";
    }

    // The page it came from, without the scheme and host: "/setup", "/files?path=/".
    String from = server->header("Referer");
    const int scheme = from.indexOf("//");
    const int pathStart = scheme >= 0 ? from.indexOf('/', scheme + 2) : -1;
    if (pathStart >= 0) from = from.substring(pathStart);
    if (from.isEmpty()) from = "?";
    LOG_DBG("WEB", "[UPLOAD] START: %s (request %u bytes) to path: %s, from %s", state.fileName.c_str(),
            static_cast<unsigned>(state.expected), state.path.c_str(), from.c_str());
    if (apMode) {
      LOG_DBG("WEB", "[UPLOAD] Free heap: %d bytes, hotspot mode", ESP.getFreeHeap());
    } else {
      LOG_DBG("WEB", "[UPLOAD] Free heap: %d bytes, Wi-Fi %d dBm", ESP.getFreeHeap(), WiFi.RSSI());
    }

    // Create file path
    String filePath = state.path;
    if (!filePath.endsWith("/")) filePath += "/";
    filePath += state.fileName;

    // Before the overwrite below removes anything. The folder is judged as a folder
    // and the target as an item, so a dot-named file is refused even where hidden
    // folders are allowed.
    if (isProtectedWebPath(state.path, Target::Directory, hiddenAllowed()) ||
        isProtectedWebPath(filePath, Target::Item, hiddenAllowed())) {
      state.error = "Cannot write to a protected location";
      LOG_DBG("WEB", "[UPLOAD] Rejected protected target: %s", filePath.c_str());
      return;
    }

    // Check if file already exists - SD operations can be slow
    HalSystem::feedWatchdog();
    if (Storage.exists(filePath.c_str())) {
      LOG_DBG("WEB", "[UPLOAD] Overwriting existing file: %s", filePath.c_str());
      HalSystem::feedWatchdog();
      Storage.remove(filePath.c_str());
    }

    // Open file for writing - this can be slow due to FAT cluster allocation
    HalSystem::feedWatchdog();
    if (!Storage.openFileForWrite("WEB", filePath, state.file)) {
      state.error = "Failed to create file on SD card";
      LOG_DBG("WEB", "[UPLOAD] FAILED to create file: %s", filePath.c_str());
      return;
    }
    HalSystem::feedWatchdog();

    LOG_DBG("WEB", "[UPLOAD] File created successfully: %s", filePath.c_str());
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (dropUploadIfCancelled()) {
      state.cancelled = true;
      return;
    }
    state.lastDataAt = millis();
    if (state.file && state.error.isEmpty()) {
      // Buffer incoming data and flush when buffer is full
      // This reduces SD card write operations and improves throughput
      const uint8_t* data = up.buf;
      size_t remaining = up.currentSize;

      while (remaining > 0) {
        const size_t space = UploadState::UPLOAD_BUFFER_SIZE - state.bufferPos;
        const size_t toCopy = (remaining < space) ? remaining : space;

        memcpy(state.buffer.data() + state.bufferPos, data, toCopy);
        state.bufferPos += toCopy;
        data += toCopy;
        remaining -= toCopy;

        // Flush buffer when full
        if (state.bufferPos >= UploadState::UPLOAD_BUFFER_SIZE) {
          if (!flushUploadBuffer(state)) {
            state.error = "Failed to write to SD card - disk may be full";
            discardUploadFile(state);
            return;
          }
        }
      }

      state.size += up.currentSize;

      // Progress every 100 KB, and every 5 s on a link too slow for that
      if (state.size - lastLoggedSize >= 102400 || state.lastDataAt - lastLoggedAt >= 5000) {
        const unsigned long elapsed = state.lastDataAt - uploadStartTime;
        const float kbps = (elapsed > 0) ? (state.size / 1024.0) / (elapsed / 1000.0) : 0;
        LOG_DBG("WEB", "[UPLOAD] %d bytes (%.1f KB), %.1f KB/s, %d writes", state.size, state.size / 1024.0, kbps,
                writeCount);
        lastLoggedSize = state.size;
        lastLoggedAt = state.lastDataAt;
      }
    }
  } else if (up.status == UPLOAD_FILE_END) {
    // A cancel on the last chunk still reaches END: WebServer raises it on the closing boundary,
    // then raises ABORTED straight after because the client is gone. ABORTED deletes only a file
    // that is still open, so finishing it here would keep the file of an aborted upload.
    if (!server->client().connected()) return;
    if (state.file) {
      // Flush any remaining buffered data
      if (!flushUploadBuffer(state)) {
        state.error = "Failed to write final data to SD card";
      }
      // close() writes the last cached sector and the directory entry: a failure there also
      // leaves a file cut short.
      if (!state.file.close() && state.error.isEmpty()) {
        state.error = "Failed to write final data to SD card";
      }
      if (!state.error.isEmpty()) discardUploadFile(state);

      if (state.error.isEmpty()) {
        state.success = true;
        const unsigned long elapsed = millis() - uploadStartTime;
        const float avgKbps = (elapsed > 0) ? (state.size / 1024.0) / (elapsed / 1000.0) : 0;
        const float writePercent = (elapsed > 0) ? (totalWriteTime * 100.0 / elapsed) : 0;
        LOG_DBG("WEB", "[UPLOAD] Complete: %s (%d bytes in %lu ms, avg %.1f KB/s)", state.fileName.c_str(), state.size,
                elapsed, avgKbps);
        LOG_DBG("WEB", "[UPLOAD] Diagnostics: %d writes, total write time: %lu ms (%.1f%%)", writeCount, totalWriteTime,
                writePercent);

        // Clear epub cache to prevent stale metadata issues when overwriting files
        String filePath = state.path;
        if (!filePath.endsWith("/")) filePath += "/";
        filePath += state.fileName;
        clearBookCacheIfNeeded(filePath);
      }
    }
    state.buffer = {};
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    state.bufferPos = 0;  // Discard buffered data
    if (state.file) discardUploadFile(state);
    state.buffer = {};
    state.error = "Upload aborted";

    // WebServer gives up on a body that stops arriving -- no byte for UPLOAD_STALL_TIMEOUT_MS
    // while the connection stays open -- or when the connection closes: the browser's doing, a
    // lost link, or Back on the device (dropUploadIfCancelled()).
    UploadState::Failure& f = state.failure;
    const unsigned long now = millis();
    f.set = true;
    f.name = state.fileName;
    f.received = state.size;
    f.expected = state.expected;
    f.ms = now - uploadStartTime;
    f.idleMs = now - state.lastDataAt;
    f.reason = state.cancelled ? "cancelled" : server->client().connected() ? "stalled" : "closed";
    f.rssi = apMode ? 0 : WiFi.RSSI();
    const float kbps = (f.ms > 0) ? (f.received / 1024.0) / (f.ms / 1000.0) : 0;
    LOG_DBG("WEB", "[UPLOAD] Aborted (%s): %s, %u of %u bytes in %lu ms (%.1f KB/s), no data for the last %lu ms",
            f.reason, f.name.c_str(), static_cast<unsigned>(f.received), static_cast<unsigned>(f.expected), f.ms, kbps,
            f.idleMs);
    if (!apMode) LOG_DBG("WEB", "[UPLOAD] Wi-Fi %d dBm", f.rssi);
  }
}

void CrossPointWebServer::handleUploadPost(UploadState& state) const {
  // Try to apply client-supplied timestamp for hotspot mode (if clock not synced from network)
  if (server->hasArg("t")) {
    const String tStr = server->arg("t");
    char* endptr = nullptr;
    errno = 0;
    long long timestamp = strtoll(tStr.c_str(), &endptr, 10);

    // Validate: full token consumed, no overflow
    if (endptr == tStr.c_str() + tStr.length() && errno == 0) {
      HalClock::applyClientTime((time_t)timestamp);
    }
  }

  if (state.success) {
    server->send(200, "text/plain", "File uploaded successfully: " + state.fileName);
  } else {
    const String error = state.error.isEmpty() ? "Unknown error during upload" : state.error;
    server->send(400, "text/plain", error);
  }
}

void CrossPointWebServer::handleCreateFolder() const {
  // Get folder name from form data
  if (!server->hasArg("name")) {
    server->send(400, "text/plain", "Missing folder name");
    return;
  }

  const String folderName = server->arg("name");

  // Validate folder name
  if (folderName.isEmpty()) {
    server->send(400, "text/plain", "Folder name cannot be empty");
    return;
  }
  // One component, never "." or "..": SdFat walks a ".." through the directory's own
  // parent entry, so an unchecked "../.crosspoint/x" would land outside the parent.
  if (!isSafePathComponent(folderName)) {
    server->send(400, "text/plain", "Invalid folder name");
    return;
  }

  // The parent it is joined to has to be resolved too, or ".." in the parent puts
  // the new folder outside it. crosspoint-reader PR #3353.
  String parentPath = "/";
  if (server->hasArg("path")) {
    parentPath = normalizeWebPath(server->arg("path"));
  }

  // Build full folder path
  String folderPath = parentPath;
  if (!folderPath.endsWith("/")) folderPath += "/";
  folderPath += folderName;

  if (isProtectedWebPath(parentPath, Target::Directory, hiddenAllowed()) ||
      isProtectedWebPath(folderPath, Target::Item, hiddenAllowed())) {
    server->send(403, "text/plain", "Cannot create a protected folder");
    return;
  }

  LOG_DBG("WEB", "Creating folder: %s", folderPath.c_str());

  // Check if already exists
  if (Storage.exists(folderPath.c_str())) {
    server->send(400, "text/plain", "Folder already exists");
    return;
  }

  // Create the folder
  if (Storage.mkdir(folderPath.c_str())) {
    LOG_DBG("WEB", "Folder created successfully: %s", folderPath.c_str());
    server->send(200, "text/plain", "Folder created: " + folderName);
  } else {
    LOG_DBG("WEB", "Failed to create folder: %s", folderPath.c_str());
    server->send(500, "text/plain", "Failed to create folder");
  }
}

void CrossPointWebServer::handleRename() const {
  if (!server->hasArg("path") || !server->hasArg("name")) {
    server->send(400, "text/plain", "Missing path or new name");
    return;
  }

  String itemPath = normalizeWebPath(server->arg("path"));
  String newName = server->arg("name");
  newName.trim();

  if (itemPath.isEmpty() || itemPath == "/") {
    server->send(400, "text/plain", "Invalid path");
    return;
  }
  if (newName.isEmpty()) {
    server->send(400, "text/plain", "New name cannot be empty");
    return;
  }
  if (newName.indexOf('/') >= 0 || newName.indexOf('\\') >= 0) {
    server->send(400, "text/plain", "Invalid file name");
    return;
  }
  if (isProtectedItemName(newName)) {
    server->send(403, "text/plain", "Cannot rename to protected name");
    return;
  }

  if (isProtectedWebPath(itemPath, Target::Item, hiddenAllowed())) {
    server->send(403, "text/plain", "Cannot rename protected item");
    return;
  }
  const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);
  if (newName == itemName) {
    server->send(200, "text/plain", "Name unchanged");
    return;
  }

  if (!Storage.exists(itemPath.c_str())) {
    server->send(404, "text/plain", "Item not found");
    return;
  }

  FsFile file = Storage.open(itemPath.c_str());
  if (!file) {
    server->send(500, "text/plain", "Failed to open file");
    return;
  }
  const bool isDir = file.isDirectory();

  String parentPath = itemPath.substring(0, itemPath.lastIndexOf('/'));
  if (parentPath.isEmpty()) {
    parentPath = "/";
  }
  String newPath = parentPath;
  if (!newPath.endsWith("/")) {
    newPath += "/";
  }
  newPath += newName;

  // The new name is ordinary (checked above), but where hidden folders are allowed
  // it can still land on a credential store's path.
  if (isProtectedWebPath(newPath, Target::Item, hiddenAllowed())) {
    file.close();
    server->send(403, "text/plain", "Cannot rename to protected name");
    return;
  }
  if (Storage.exists(newPath.c_str())) {
    file.close();
    server->send(409, "text/plain", "Target already exists");
    return;
  }

  const bool success = file.rename(newPath.c_str());
  file.close();

  if (success) {
    LOG_DBG("WEB", "Renamed: %s -> %s", itemPath.c_str(), newPath.c_str());
    // The books keep their reading position, bookmarks and history. They used to lose them: the
    // caches were cleared before the rename, as if the books were new.
    if (isDir) {
      BookRelocation::folderMoved(itemPath.c_str(), newPath.c_str());
    } else {
      BookRelocation::bookMoved(itemPath.c_str(), newPath.c_str());
    }
    server->send(200, "text/plain", "Renamed successfully");
  } else {
    LOG_ERR("WEB", "Failed to rename: %s -> %s", itemPath.c_str(), newPath.c_str());
    server->send(500, "text/plain", "Failed to rename");
  }
}

void CrossPointWebServer::handleMove() const {
  if (!server->hasArg("path") || !server->hasArg("dest")) {
    server->send(400, "text/plain", "Missing path or destination");
    return;
  }

  String itemPath = normalizeWebPath(server->arg("path"));
  String destPath = normalizeWebPath(server->arg("dest"));

  if (itemPath.isEmpty() || itemPath == "/") {
    server->send(400, "text/plain", "Invalid path");
    return;
  }
  if (destPath.isEmpty()) {
    server->send(400, "text/plain", "Invalid destination");
    return;
  }

  const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);
  String newPath = destPath;
  if (!newPath.endsWith("/")) {
    newPath += "/";
  }
  newPath += itemName;
  // Source, destination folder and where the item lands: a move is a write into
  // the destination as much as a removal from the source.
  if (isProtectedWebPath(itemPath, Target::Item, hiddenAllowed()) ||
      isProtectedWebPath(destPath, Target::Directory, hiddenAllowed()) ||
      isProtectedWebPath(newPath, Target::Item, hiddenAllowed())) {
    server->send(403, "text/plain", "Cannot move protected item");
    return;
  }

  if (!Storage.exists(itemPath.c_str())) {
    server->send(404, "text/plain", "Item not found");
    return;
  }

  FsFile file = Storage.open(itemPath.c_str());
  if (!file) {
    server->send(500, "text/plain", "Failed to open file");
    return;
  }
  const bool isDir = file.isDirectory();

  if (isDir) {
    String destWithSlash = destPath;
    if (!destWithSlash.endsWith("/")) destWithSlash += "/";
    String itemWithSlash = itemPath;
    if (!itemWithSlash.endsWith("/")) itemWithSlash += "/";
    if (destPath == itemPath || destWithSlash.startsWith(itemWithSlash)) {
      file.close();
      server->send(400, "text/plain", "Cannot move folder into itself");
      return;
    }
  }

  if (!Storage.exists(destPath.c_str())) {
    file.close();
    server->send(404, "text/plain", "Destination not found");
    return;
  }
  FsFile destDir = Storage.open(destPath.c_str());
  if (!destDir || !destDir.isDirectory()) {
    if (destDir) {
      destDir.close();
    }
    file.close();
    server->send(400, "text/plain", "Destination is not a folder");
    return;
  }
  destDir.close();

  if (newPath == itemPath) {
    file.close();
    server->send(200, "text/plain", "Already in destination");
    return;
  }
  if (Storage.exists(newPath.c_str())) {
    file.close();
    server->send(409, "text/plain", "Target already exists");
    return;
  }

  const bool success = file.rename(newPath.c_str());
  file.close();

  if (success) {
    LOG_DBG("WEB", "Moved: %s -> %s", itemPath.c_str(), newPath.c_str());
    if (isDir) {
      BookRelocation::folderMoved(itemPath.c_str(), newPath.c_str());
    } else {
      BookRelocation::bookMoved(itemPath.c_str(), newPath.c_str());
    }
    server->send(200, "text/plain", "Moved successfully");
  } else {
    LOG_ERR("WEB", "Failed to move: %s -> %s", itemPath.c_str(), newPath.c_str());
    server->send(500, "text/plain", "Failed to move");
  }
}

void CrossPointWebServer::handleDelete() const {
  // To ensure backwards compatibility, plain `path` is mapped
  // to a single element JSON array.
  bool hasPathArg = server->hasArg("path");
  bool hasPathsArg = server->hasArg("paths");
  // Check 'paths' or `path` argument is provided
  if (!(hasPathArg || hasPathsArg)) {
    server->send(400, "text/plain", "Missing `path` or `paths` argument");
    return;
  }
  if (hasPathArg && hasPathsArg) {
    server->send(400, "text/plain", "Provide either 'path' or 'paths', not both");
    return;
  }

  // Parse paths
  String pathsArg;
  JsonDocument doc;
  DeserializationError error = DeserializationError(DeserializationError::Code::Ok);
  if (hasPathsArg) {
    pathsArg = server->arg("paths");
    error = deserializeJson(doc, pathsArg);
  } else {
    pathsArg = server->arg("path");
    doc.add(pathsArg);
  }
  if (error) {
    server->send(400, "text/plain", "Invalid paths format");
    return;
  }

  auto paths = doc.as<JsonArray>();
  if (paths.isNull() || paths.size() == 0) {
    server->send(400, "text/plain", "No paths provided");
    return;
  }

  // Iterate over paths and delete each item
  bool allSuccess = true;
  String failedItems;
  size_t deletedCount = 0;
  String lastDeletedItem;

  for (const auto& p : paths) {
    // Resolved before the protection check below -- see handleFileListData.
    // crosspoint-reader PR #3353.
    auto itemPath = normalizeWebPath(p.as<String>());

    // Validate path
    if (itemPath.isEmpty() || itemPath == "/") {
      failedItems += itemPath + " (cannot delete root); ";
      allSuccess = false;
      continue;
    }

    if (isProtectedWebPath(itemPath, Target::Item, hiddenAllowed())) {
      failedItems += itemPath + " (protected item); ";
      allSuccess = false;
      continue;
    }

    // Check if item exists
    if (!Storage.exists(itemPath.c_str())) {
      failedItems += itemPath + " (not found); ";
      allSuccess = false;
      continue;
    }

    // Decide whether it's a directory or file by opening it
    bool success = false;
    FsFile f = Storage.open(itemPath.c_str());
    if (f && f.isDirectory()) {
      // For folders, ensure empty before removing
      FsFile entry = f.openNextFile();
      if (entry) {
        entry.close();
        f.close();
        failedItems += itemPath + " (folder not empty); ";
        allSuccess = false;
        continue;
      }
      f.close();
      success = Storage.rmdir(itemPath.c_str());
    } else {
      // It's a file (or couldn't open as dir) — remove file
      if (f) f.close();
      success = Storage.remove(itemPath.c_str());
      clearBookCacheIfNeeded(itemPath);
    }

    if (!success) {
      failedItems += itemPath + " (deletion failed); ";
      allSuccess = false;
    } else {
      deletedCount++;
      lastDeletedItem = itemPath.substring(itemPath.lastIndexOf('/') + 1);
    }
  }

  if (deletedCount > 0) {
    lastDeleteName = lastDeletedItem;
    lastDeleteCount = deletedCount;
    lastDeleteAt = millis();
  }

  if (allSuccess) {
    server->send(200, "text/plain", "All items deleted successfully");
  } else {
    server->send(500, "text/plain", "Failed to delete some items: " + failedItems);
  }
}

void CrossPointWebServer::handleSettingsPage() const { serveBundlePage(web_bundle::SettingsPageHtml); }

void CrossPointWebServer::handleGetSettings() const {
  if (rejectIfLowMemory(server.get())) return;
  LOG_WEB_MEM("settings_enter");

  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  ChunkedJsonArray out(server.get());

  JsonDocument doc;

  // Straight into the response, without a buffer of its own: an entry can be any size. The
  // timezone row's 86 city names make about 2 KB, more than the 1 KB buffer this used to be
  // formatted into, so the row was dropped and the web page had no Timezone setting.
  auto sendEntry = [&]() { serializeJson(doc, out.nextEntry()); };

  // Rows are visited one at a time (forEachSetting()), never held as a list: the whole list is
  // one 11.5 KB block, and this runs while Wi-Fi holds most of the heap.
  forEachSetting([&](const SettingInfo& sBase) {
    if (!sBase.key) return;  // Skip ACTION-only entries

    // Enrich font-family entries with current SD card families.
    SettingInfo sLocal;
    const SettingInfo* sPtr = &sBase;
    if (std::strcmp(sBase.key, "fontFamily") == 0 || std::strcmp(sBase.key, "txtFontFamily") == 0) {
      sLocal = sBase;
      const uint8_t n = fontFamilyOptionCount();
      sLocal.enumLabels.clear();
      sLocal.enumLabels.reserve(n);
      for (uint8_t i = 0; i < n; i++) sLocal.enumLabels.push_back(fontFamilyOptionLabel(i));
      sPtr = &sLocal;
    }
    const SettingInfo& s = *sPtr;

    doc.clear();
    doc["key"] = s.key;
    doc["name"] = I18N.get(s.nameId);
    doc["category"] = I18N.get(s.category);
    doc["subcategory"] = s.subcategory != StrId::STR_NONE_OPT ? I18N.get(s.subcategory) : "";
    doc["submenu"] = s.submenu != StrId::STR_NONE_OPT ? I18N.get(s.submenu) : "";

    switch (s.type) {
      case SettingType::TOGGLE: {
        doc["type"] = "toggle";
        if (s.valuePtr) {
          doc["value"] = static_cast<int>(SETTINGS.*(s.valuePtr));
        } else if (s.valueGetter) {
          doc["value"] = static_cast<int>(s.callValueGetter());
        }
        break;
      }
      case SettingType::ENUM: {
        doc["type"] = "enum";
        if (s.valuePtr) {
          doc["value"] = static_cast<int>(SETTINGS.*(s.valuePtr));
        } else if (s.valueGetter) {
          doc["value"] = static_cast<int>(s.callValueGetter());
        }
        JsonArray options = doc["options"].to<JsonArray>();
        // Through the shared accessors, not the vectors. A row may draw its options from
        // enumValues, from enumLabels, or from a flash-resident literal callback that extends
        // enumValues (the timezone row's 86 city names), and reading a vector directly renders
        // an empty dropdown for any form it does not happen to know about.
        //
        // Flash pointer first: ArduinoJson stores a const char* by reference and copies a
        // std::string, so this keeps the document holding pointers rather than 1.7 KB of copied
        // city names. Rows whose labels only exist as std::strings fall back to the copy.
        const uint8_t optionCount = s.getEnumOptionCount();
        for (uint8_t i = 0; i < optionCount; i++) {
          if (const char* flash = s.getEnumOptionFlashLabel(i)) {
            options.add(flash);
          } else {
            options.add(s.getEnumOptionLabel(i));
          }
        }
        break;
      }
      case SettingType::VALUE: {
        doc["type"] = "value";
        if (s.valuePtr) {
          doc["value"] = static_cast<int>(SETTINGS.*(s.valuePtr));
        } else if (s.valueGetter) {
          doc["value"] = static_cast<int>(s.callValueGetter());
        }
        doc["min"] = s.valueRange.min;
        doc["max"] = s.valueRange.max;
        doc["step"] = s.valueRange.step;
        break;
      }
      case SettingType::STRING: {
        doc["type"] = "string";
        // Obfuscated settings (credentials) are write-only over the wire: the value is
        // never disclosed to any client on the network, but the field stays editable so
        // a new one can be typed in the browser instead of on the device keyboard.
        // Sending it also risks invalid UTF-8 — a password may contain arbitrary bytes,
        // which serialize fine here but make response.json() throw in the browser.
        // "isSet" lets the UI distinguish "a password is stored" from "none stored"
        // without revealing it; the client pairs it with an explicit clear control so
        // an empty field can mean "leave unchanged" rather than "erase".
        if (s.obfuscated) {
          bool isSet = false;
          if (s.stringGetter) {
            isSet = !s.callStringGetter().empty();
          } else if (s.stringMaxLen > 0) {
            isSet = *(reinterpret_cast<const char*>(&SETTINGS) + s.stringOffset) != '\0';
          }
          doc["value"] = "";
          doc["obfuscated"] = true;
          doc["isSet"] = isSet;
        } else if (s.stringGetter) {
          doc["value"] = s.callStringGetter();
        } else if (s.stringMaxLen > 0) {
          doc["value"] = reinterpret_cast<const char*>(&SETTINGS) + s.stringOffset;
        }
        break;
      }
      default:
        return;
    }

    sendEntry();
  });

  // Expose sleepTimeoutMinutes and refreshFrequencyPages as VALUE entries.
  auto appendValueSetting = [&](const char* key, const char* name, const char* category, const char* subcategory,
                                int value, int min, int max, int step) {
    doc.clear();
    doc["key"] = key;
    doc["name"] = name;
    doc["category"] = category;
    doc["subcategory"] = subcategory;
    doc["submenu"] = "";
    doc["type"] = "value";
    doc["value"] = value;
    doc["min"] = min;
    doc["max"] = max;
    doc["step"] = step;
    sendEntry();
  };
  appendValueSetting("sleepTimeoutMinutes", I18N.get(StrId::STR_TIME_TO_SLEEP), I18N.get(StrId::STR_CAT_DISPLAY), "",
                     SETTINGS.sleepTimeoutMinutes, 0, 60, 1);
  appendValueSetting("refreshFrequencyPages", I18N.get(StrId::STR_REFRESH_FREQ), I18N.get(StrId::STR_CAT_DISPLAY),
                     I18N.get(StrId::STR_MENU_DISP_REFRESH), SETTINGS.refreshFrequencyPages, 0, 60, 1);

  out.finish();
  LOG_WEB_MEM("settings_exit");
  LOG_DBG("WEB", "Served settings API");
}

void CrossPointWebServer::handlePostSettings() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  // One row at a time, as in handleGetSettings().
  int applied = 0;

  const auto isFontFamilyKey = [](const char* key) {
    return key && (std::strcmp(key, "fontFamily") == 0 || std::strcmp(key, "txtFontFamily") == 0);
  };
  const auto applyRow = [&](const SettingInfo& s) {
    if (!s.key) return;
    if (!doc[s.key].is<JsonVariant>()) return;

    switch (s.type) {
      case SettingType::TOGGLE: {
        const int val = doc[s.key].as<int>() ? 1 : 0;
        if (s.valuePtr) {
          SETTINGS.*(s.valuePtr) = val;
        } else if (s.valueSetter) {
          s.callValueSetter(static_cast<uint8_t>(val));
        }
        applied++;
        break;
      }
      case SettingType::ENUM: {
        const int val = doc[s.key].as<int>();
        // For font-family keys the enumLabels in the static list are empty by design
        // (built lazily by handleGetSettings); use the dynamic option count instead.
        const bool familyKey = isFontFamilyKey(s.key);
        // Otherwise getEnumOptionCount(), for the same reason as the options array above: it is
        // the one definition of how many options a row has, whichever form they come in.
        const int count =
            familyKey ? static_cast<int>(fontFamilyOptionCount()) : static_cast<int>(s.getEnumOptionCount());
        if (val >= 0 && val < count) {
          if (s.valuePtr) {
            SETTINGS.*(s.valuePtr) = static_cast<uint8_t>(val);
          } else if (s.valueSetter) {
            s.callValueSetter(static_cast<uint8_t>(val));
          }
          applied++;
        }
        break;
      }
      case SettingType::VALUE: {
        const int val = doc[s.key].as<int>();
        if (val >= s.valueRange.min && val <= s.valueRange.max) {
          if (s.valuePtr) {
            SETTINGS.*(s.valuePtr) = static_cast<uint8_t>(val);
          } else if (s.valueSetter) {
            s.callValueSetter(static_cast<uint8_t>(val));
          }
          applied++;
        }
        break;
      }
      case SettingType::STRING: {
        const std::string val = doc[s.key].as<std::string>();
        if (s.stringSetter) {
          s.callStringSetter(val);
        } else if (s.stringMaxLen > 0) {
          char* ptr = reinterpret_cast<char*>(&SETTINGS) + s.stringOffset;
          strncpy(ptr, val.c_str(), s.stringMaxLen - 1);
          ptr[s.stringMaxLen - 1] = '\0';
        }
        applied++;
        break;
      }
      default:
        break;
    }
  };

  // Families last. A font-size value is an index into the sizes of the family the page was showing,
  // which is the one selected now, so it has to be applied before this request changes that
  // family. Two walks rather than a reordered list: rows are visited one at a time, never held.
  forEachSetting([&](const SettingInfo& s) {
    if (!isFontFamilyKey(s.key)) applyRow(s);
  });
  forEachSetting([&](const SettingInfo& s) {
    if (isFontFamilyKey(s.key)) applyRow(s);
  });

  // Handle sleepTimeoutMinutes and refreshFrequencyPages posted as VALUE types.
  if (doc["sleepTimeoutMinutes"].is<int>()) {
    const int v = doc["sleepTimeoutMinutes"].as<int>();
    if (v >= 0 && v <= 60) {
      SETTINGS.sleepTimeoutMinutes = static_cast<uint8_t>(v);
      applied++;
    }
  }
  if (doc["refreshFrequencyPages"].is<int>()) {
    const int v = doc["refreshFrequencyPages"].as<int>();
    if (v >= 0 && v <= 60) {
      SETTINGS.refreshFrequencyPages = static_cast<uint8_t>(v);
      applied++;
    }
  }

  CrossPointSettings::normalizeDependentSettings(SETTINGS);
  SETTINGS.saveToFile();

  LOG_DBG("WEB", "Applied %d setting(s)", applied);
  server->send(200, "text/plain", String("Applied ") + String(applied) + " setting(s)");
}

// ---- Font Management API ----

namespace {
// Cap chosen so that FONTS_DIR + "/" + family + "__staging/" + filename stays
// well under the 128-byte path buffers used below. snprintf truncation checks
// are the actual guard; this just rejects obviously-bogus entries early.
static constexpr size_t MAX_FONT_FILE_NAME_LEN = 60;

bool isValidFontFileName(const char* name) {
  const size_t len = strlen(name);
  if (len == 0 || len > MAX_FONT_FILE_NAME_LEN) return false;
  // Allow '/' for subdirectories of font families, but prevent absolute paths
  if (name[0] == '/') return false;
  if (strchr(name, '\\') != nullptr) return false;
  if (strstr(name, "..") != nullptr) return false;
  return true;
}

bool fetchRemoteFontManifest(HttpDownloader::Session& session, FontInstaller& installer, FontCatalog& outFamilies,
                             std::string& outBaseUrl, std::string& outError) {
  static constexpr const char* MANIFEST_TMP = "/fonts_manifest_web.tmp";

  auto result = HttpDownloader::downloadToFile(session, FONT_MANIFEST_URL, MANIFEST_TMP, nullptr);
  if (result != HttpDownloader::OK) {
    outError = "Failed to fetch font manifest";
    Storage.remove(MANIFEST_TMP);
    return false;
  }

  FsFile manifestFile;
  if (!Storage.openFileForRead("WEB", MANIFEST_TMP, manifestFile)) {
    outError = "Failed to open downloaded manifest";
    Storage.remove(MANIFEST_TMP);
    return false;
  }

  // Streamed off the card a block at a time, never the whole document: this
  // runs while Wi-Fi is up, and a whole-document parse of the 24 KB manifest
  // ran the X3 out of heap. The list is one block, which the install frees
  // whole before its downloads -- see handleFontDownload().
  FontManifestReader reader("WEB", FontInstaller::isValidFamilyName, isValidFontFileName);
  const FontManifestStatus status = reader.read(manifestFile, outFamilies, outBaseUrl);
  manifestFile.close();
  Storage.remove(MANIFEST_TMP);
  if (status == FontManifestStatus::Invalid) {
    outError = "Failed to parse font manifest";
    return false;
  }
  if (status == FontManifestStatus::UnsupportedVersion) {
    outError = "Unsupported manifest version";
    return false;
  }
  if (status == FontManifestStatus::OutOfMemory) {
    LOG_ERR("WEB", "Font manifest: %s", reader.failure());
    outError = "Failed to read font manifest: out of memory";
    return false;
  }

  for (size_t i = 0; i < outFamilies.count(); i++) {
    const char* familyName = outFamilies.name(i);
    outFamilies.setInstalled(i, installer.isFamilyInstalled(familyName));
    outFamilies.setHasUpdate(i, false);
    if (outFamilies.installed(i)) {
      for (size_t j = 0; j < outFamilies.fileCount(i); j++) {
        char path[128];
        FontInstaller::buildFontPath(familyName, outFamilies.fileLocalName(i, j), path, sizeof(path));
        FsFile f;
        if (Storage.openFileForRead("WEB", path, f)) {
          const size_t actual = static_cast<size_t>(f.size());
          f.close();
          if (actual != outFamilies.fileSize(i, j)) {
            outFamilies.setHasUpdate(i, true);
            break;
          }
        } else {
          outFamilies.setHasUpdate(i, true);
          break;
        }
      }
    }
  }

  return true;
}

// Installs the family at `familyIndex` of `families`.
bool installRemoteFamily(HttpDownloader::Session& session, const FontCatalog& families, const size_t familyIndex,
                         const std::string& baseUrl, FontInstaller& installer, std::string& outError) {
  const char* familyName = families.name(familyIndex);
  const size_t fileCount = families.fileCount(familyIndex);
  if (!FontInstaller::isValidFamilyName(familyName)) {
    outError = "Invalid family name";
    return false;
  }
  for (size_t j = 0; j < fileCount; j++) {
    if (!isValidFontFileName(families.fileName(familyIndex, j))) {
      outError = std::string("Invalid file name: ") + families.fileName(familyIndex, j);
      return false;
    }
  }

  char liveDir[128];
  char stagingDir[128];
  char backupDir[128];
  int n;
  n = snprintf(liveDir, sizeof(liveDir), "%s/%s", SdCardFontRegistry::FONTS_DIR, familyName);
  if (n < 0 || static_cast<size_t>(n) >= sizeof(liveDir)) {
    outError = "Family path too long";
    return false;
  }
  n = snprintf(stagingDir, sizeof(stagingDir), "%s/%s__staging", SdCardFontRegistry::FONTS_DIR, familyName);
  if (n < 0 || static_cast<size_t>(n) >= sizeof(stagingDir)) {
    outError = "Family path too long";
    return false;
  }
  n = snprintf(backupDir, sizeof(backupDir), "%s/%s__backup", SdCardFontRegistry::FONTS_DIR, familyName);
  if (n < 0 || static_cast<size_t>(n) >= sizeof(backupDir)) {
    outError = "Family path too long";
    return false;
  }

  // Resumable staging: keep an existing __staging dir from a prior attempt so
  // already-downloaded files can be reused (size + CRC validated per file).
  if (!Storage.exists(stagingDir) && !Storage.mkdir(stagingDir)) {
    outError = "Failed to create staging area";
    return false;
  }

  // The session is owned by the caller (handleFontInstall) so it can be
  // reused across the manifest fetch and every family install in one batch.
  for (size_t j = 0; j < fileCount; j++) {
    HalSystem::feedWatchdog();
    yield();

    const char* fileName = families.fileName(familyIndex, j);
    const char* localFilename = families.fileLocalName(familyIndex, j);
    const uint32_t expectedSize = families.fileSize(familyIndex, j);
    const bool hasCrc32 = families.fileHasCrc32(familyIndex, j);
    const uint32_t expectedCrc32 = families.fileCrc32(familyIndex, j);

    char stagedPath[128];
    int sn = snprintf(stagedPath, sizeof(stagedPath), "%s/%s", stagingDir, localFilename);
    if (sn < 0 || static_cast<size_t>(sn) >= sizeof(stagedPath)) {
      // Path-length bugs are not resumable; nuke staging so we don't get stuck.
      Storage.removeDir(stagingDir);
      outError = std::string("File path too long: ") + localFilename;
      return false;
    }

    // Reuse a previously-downloaded file if it still matches the manifest.
    if (Storage.exists(stagedPath)) {
      FsFile f;
      bool sizeOk = false;
      if (Storage.openFileForRead("WEB", stagedPath, f)) {
        sizeOk = (static_cast<size_t>(f.size()) == expectedSize);
        f.close();
      }
      bool crcOk = !hasCrc32;
      if (sizeOk && hasCrc32) {
        uint32_t actualCrc = 0;
        if (FontInstaller::computeFileCrc32(stagedPath, actualCrc)) {
          crcOk = (actualCrc == expectedCrc32);
        }
      }
      if (sizeOk && crcOk && installer.validateCpfontFile(stagedPath)) {
        LOG_DBG("WEB", "Resuming: reusing %s", stagedPath);
        continue;
      }
      Storage.remove(stagedPath);
    }

    // Ensure intermediate subdirectories exist inside stagingDir
    std::string stagedPathStr(stagedPath);
    size_t lastSlash = stagedPathStr.find_last_of('/');
    if (lastSlash != std::string::npos) {
      Storage.mkdir(stagedPathStr.substr(0, lastSlash).c_str());
    }

    const std::string url = baseUrl + fileName;

    auto result = HttpDownloader::downloadToFile(session, url, stagedPath, nullptr);
    if (result != HttpDownloader::OK) {
      // Drop just the failed file; keep already-downloaded siblings.
      Storage.remove(stagedPath);
      outError = std::string("Download failed: ") + fileName;
      return false;
    }

    if (hasCrc32) {
      uint32_t actualCrc = 0;
      if (!FontInstaller::computeFileCrc32(stagedPath, actualCrc) || actualCrc != expectedCrc32) {
        Storage.remove(stagedPath);
        outError = std::string("Checksum mismatch: ") + fileName;
        return false;
      }
    }

    if (!installer.validateCpfontFile(stagedPath)) {
      Storage.remove(stagedPath);
      outError = std::string("Invalid font file: ") + fileName;
      return false;
    }
  }

  const bool hadLiveDir = Storage.exists(liveDir);
  if (Storage.exists(backupDir) && !Storage.removeDir(backupDir)) {
    Storage.removeDir(stagingDir);
    outError = "Failed to prepare backup area";
    return false;
  }
  if (hadLiveDir && !Storage.rename(liveDir, backupDir)) {
    Storage.removeDir(stagingDir);
    outError = "Failed to replace installed font";
    return false;
  }
  if (!Storage.rename(stagingDir, liveDir)) {
    if (hadLiveDir && Storage.exists(backupDir)) {
      Storage.rename(backupDir, liveDir);
    }
    Storage.removeDir(stagingDir);
    outError = "Failed to finalize font install";
    return false;
  }
  if (Storage.exists(backupDir)) {
    Storage.removeDir(backupDir);
  }

  return true;
}
}  // namespace

void CrossPointWebServer::handleFontsPage() const { serveBundlePage(web_bundle::FontsPageHtml); }

// The page unpacks and checks a dictionary in the browser and writes it through /mkdir,
// /upload and /delete: the device has neither the heap to inflate a .idx.gz nor any reason
// to learn archive formats, so serving the page is all the firmware does.
void CrossPointWebServer::handleDictionaryPage() const { serveBundlePage(web_bundle::DictionaryPageHtml); }

void CrossPointWebServer::handleFontList() {
  if (rejectIfLowMemory(server.get())) return;
  LOG_WEB_MEM("font_list_enter");
  FontInstaller installer(sdFontSystem.registry());
  installer.refreshRegistry();
  const auto& families = sdFontSystem.registry().getFamilies();

  JsonDocument doc;
  JsonArray arr = doc["families"].to<JsonArray>();
  doc["maxFamilies"] = SdCardFontRegistry::MAX_SD_FAMILIES;

  for (const auto& family : families) {
    JsonObject fObj = arr.add<JsonObject>();
    fObj["name"] = family.name;

    JsonArray sizes = fObj["sizes"].to<JsonArray>();
    for (uint8_t s : family.availableSizes()) {
      sizes.add(s);
    }

    JsonArray files = fObj["files"].to<JsonArray>();
    for (const auto& file : family.files) {
      JsonObject fileObj = files.add<JsonObject>();
      const char* name = strrchr(file.path.c_str(), '/');
      fileObj["name"] = name ? name + 1 : file.path.c_str();

      FsFile f;
      if (Storage.openFileForRead("WEB", file.path.c_str(), f)) {
        fileObj["size"] = static_cast<unsigned long>(f.size());
        f.close();
      } else {
        fileObj["size"] = 0;
      }
    }
  }

  LOG_WEB_MEM("font_list_exit");
  sendJson(server.get(), 200, doc);
}

void CrossPointWebServer::handleFontManifest() {
  if (rejectIfLowMemory(server.get())) return;
  LOG_WEB_MEM("font_manifest_enter");
  FontInstaller installer(sdFontSystem.registry());
  installer.refreshRegistry();

  FontCatalog families;
  std::string baseUrl;
  std::string error;
  {
    HttpDownloader::Session manifestSession;
    if (!fetchRemoteFontManifest(manifestSession, installer, families, baseUrl, error)) {
      JsonDocument errDoc;
      errDoc["ok"] = false;
      errDoc["error"] = error;
      sendJson(server.get(), 500, errDoc);
      return;
    }
  }  // close TLS before the response JSON document is built

  JsonDocument doc;
  doc["ok"] = true;
  doc["baseUrl"] = baseUrl;
  JsonArray arr = doc["families"].to<JsonArray>();
  for (size_t i = 0; i < families.count(); i++) {
    JsonObject obj = arr.add<JsonObject>();
    obj["name"] = families.name(i);
    obj["description"] = families.description(i);
    obj["installed"] = families.installed(i);
    obj["hasUpdate"] = families.hasUpdate(i);
    obj["totalSize"] = static_cast<unsigned long>(families.totalSize(i));
    obj["fileCount"] = static_cast<unsigned>(families.fileCount(i));
  }
  LOG_WEB_MEM("font_manifest_exit");
  sendJson(server.get(), 200, doc);
}

void CrossPointWebServer::handleFontDownload() {
  String body = server->arg("plain");
  JsonDocument req;
  if (deserializeJson(req, body)) {
    server->send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid request\"}");
    return;
  }
  body.clear();  // Free memory early!

  const bool installAll = req["all"].as<bool>();
  const std::string requestedFamily = req["family"] | "";
  if (!installAll && requestedFamily.empty()) {
    server->send(400, "application/json", "{\"ok\":false,\"error\":\"Missing family\"}");
    return;
  }

  FontInstaller installer(sdFontSystem.registry());
  installer.refreshRegistry();

  // The manifest fetch uses its own session, closed at the end of the block
  // below, before a separate install session is opened for the actual family
  // downloads.
  FontCatalog families;
  std::string baseUrl;
  std::string error;
  {
    HttpDownloader::Session manifestSession;
    if (!fetchRemoteFontManifest(manifestSession, installer, families, baseUrl, error)) {
      JsonDocument errDoc;
      errDoc["ok"] = false;
      errDoc["error"] = error;
      sendJson(server.get(), 500, errDoc);
      return;
    }
  }  // manifestSession destructor closes the TLS connection here

  // The families to install, copied into a block of their own, so the whole
  // list can be freed before the downloads: their TLS session needs a
  // contiguous record buffer, and the list's block comes back in one piece.
  FontCatalog targets;
  bool copied = false;
  if (installAll) {
    copied = targets.copyFrom(
        families,
        [](const void*, const FontCatalog& from, const size_t i) { return !from.installed(i) || from.hasUpdate(i); },
        nullptr);
  } else {
    size_t found = families.count();
    for (size_t i = 0; i < families.count(); i++) {
      if (requestedFamily == families.name(i)) {
        found = i;
        break;
      }
    }
    if (found == families.count()) {
      server->send(404, "application/json", "{\"ok\":false,\"error\":\"Family not found in manifest\"}");
      return;
    }
    copied = targets.copyFamilyFrom(families, found);
  }
  families.clear();
  if (!copied) {
    LOG_ERR("WEB", "OOM: copy of the fonts to install");
    server->send(500, "application/json", "{\"ok\":false,\"error\":\"Out of memory\"}");
    return;
  }

  // One install session covers every family in this batch. TLS handshake
  // happens once on the first file of the first family; subsequent files
  // (within and across families) reuse the open keep-alive connection.
  HttpDownloader::Session installSession;

  size_t installedCount = 0;
  for (size_t i = 0; i < targets.count(); i++) {
    HalSystem::feedWatchdog();
    yield();

    LOG_DBG("WEB", "Installing font family: %s", targets.name(i));

    if (!installRemoteFamily(installSession, targets, i, baseUrl, installer, error)) {
      JsonDocument errDoc;
      errDoc["ok"] = false;
      errDoc["error"] = error;
      errDoc["family"] = targets.name(i);
      errDoc["installedCount"] = static_cast<unsigned>(installedCount);
      sendJson(server.get(), 500, errDoc);
      return;
    }
    installedCount++;
  }

  installer.refreshRegistry();
  JsonDocument res;
  res["ok"] = true;
  res["installedCount"] = static_cast<unsigned>(installedCount);
  sendJson(server.get(), 200, res);
}

void CrossPointWebServer::handleFontUploadData() {
  HTTPUpload& up = server->upload();

  switch (up.status) {
    case UPLOAD_FILE_START: {
      HalSystem::feedWatchdog();
      tolerateUploadStalls(*server);
      String family = server->arg("family");
      fontUpload.valid = false;
      fontUpload.magicChecked = false;
      fontUpload.headerBytesReceived = 0;
      fontUpload.bytesWritten = 0;
      fontUpload.bufferPos = 0;
      fontUpload.buffer.resize(FontUploadState::BUFFER_SIZE);
      // The END and ABORTED cases delete filePath whenever the upload is not valid. Left
      // over from the previous upload, it named that upload's INSTALLED font, so a START
      // refused below (bad family or file name, no family dir) deleted it.
      fontUpload.filePath.clear();

      if (!FontInstaller::isValidFamilyName(family.c_str())) {
        LOG_ERR("WEB", "Invalid font family name: %s", family.c_str());
        break;
      }

      String filename = up.filename;
      if (!filename.endsWith(".cpfont")) {
        LOG_ERR("WEB", "Not a .cpfont file: %s", filename.c_str());
        break;
      }
      if (filename.indexOf('/') >= 0 || filename.indexOf('\\') >= 0 || filename.indexOf("..") >= 0) {
        LOG_ERR("WEB", "Invalid font filename: %s", filename.c_str());
        break;
      }

      fontUpload.familyName = family.c_str();

      FontInstaller installer(sdFontSystem.registry());
      if (!installer.ensureFamilyDir(family.c_str())) {
        LOG_ERR("WEB", "Failed to create font family dir");
        break;
      }

      char path[128];
      FontInstaller::buildFontPath(family.c_str(), filename.c_str(), path, sizeof(path));
      fontUpload.filePath = path;

      if (!Storage.openFileForWrite("WEB", path, fontUpload.file)) {
        LOG_ERR("WEB", "Failed to open font file for write: %s", path);
        break;
      }

      fontUpload.valid = true;
      LOG_DBG("WEB", "Font upload started: %s -> %s", filename.c_str(), path);
      break;
    }

    case UPLOAD_FILE_WRITE: {
      // No END guard needed here, unlike /upload: this ABORTED branch deletes the file whether or
      // not END already closed it.
      if (dropUploadIfCancelled() || !fontUpload.valid) break;
      HalSystem::feedWatchdog();

      if (!fontUpload.magicChecked) {
        size_t needed = 8 - fontUpload.headerBytesReceived;
        size_t take = (up.currentSize < needed) ? up.currentSize : needed;
        if (take > 0) {
          memcpy(fontUpload.header + fontUpload.headerBytesReceived, up.buf, take);
          fontUpload.headerBytesReceived += take;
        }
        if (fontUpload.headerBytesReceived == 8) {
          if (memcmp(fontUpload.header, "CPFONT\0\0", 8) != 0) {
            LOG_ERR("WEB", "Invalid .cpfont magic bytes");
            fontUpload.valid = false;
            fontUpload.file.close();
            return;
          }
          fontUpload.magicChecked = true;
        }
      }

      size_t remaining = up.currentSize;
      const uint8_t* src = up.buf;
      while (remaining > 0) {
        size_t space = FontUploadState::BUFFER_SIZE - fontUpload.bufferPos;
        size_t chunk = (remaining < space) ? remaining : space;
        memcpy(fontUpload.buffer.data() + fontUpload.bufferPos, src, chunk);
        fontUpload.bufferPos += chunk;
        src += chunk;
        remaining -= chunk;

        if (fontUpload.bufferPos >= FontUploadState::BUFFER_SIZE) {
          const size_t expected = fontUpload.bufferPos;
          const size_t written = fontUpload.file.write(fontUpload.buffer.data(), expected);
          fontUpload.bytesWritten += written;
          if (written != expected) {
            LOG_ERR("WEB", "Failed writing uploaded font chunk (%u/%u bytes)", static_cast<unsigned>(written),
                    static_cast<unsigned>(expected));
            fontUpload.valid = false;
            fontUpload.file.close();
            return;
          }
          fontUpload.bufferPos = 0;
          HalSystem::feedWatchdog();
        }
      }
      break;
    }

    case UPLOAD_FILE_END: {
      if (fontUpload.valid && !fontUpload.magicChecked) {
        LOG_ERR("WEB", "Invalid .cpfont upload: header not fully received");
        fontUpload.valid = false;
      }
      if (fontUpload.valid && fontUpload.bufferPos > 0) {
        const size_t expected = fontUpload.bufferPos;
        const size_t written = fontUpload.file.write(fontUpload.buffer.data(), expected);
        fontUpload.bytesWritten += written;
        if (written != expected) {
          LOG_ERR("WEB", "Failed flushing uploaded font chunk (%u/%u bytes)", static_cast<unsigned>(written),
                  static_cast<unsigned>(expected));
          fontUpload.valid = false;
        }
        fontUpload.bufferPos = 0;
      }
      fontUpload.file.close();

      if (!fontUpload.valid && !fontUpload.filePath.empty()) {
        Storage.remove(fontUpload.filePath.c_str());
      }

      fontUpload.buffer = {};
      LOG_DBG("WEB", "Font upload end: valid=%d, %zu bytes", fontUpload.valid, fontUpload.bytesWritten);
      break;
    }

    case UPLOAD_FILE_ABORTED: {
      fontUpload.file.close();
      if (!fontUpload.filePath.empty()) {
        Storage.remove(fontUpload.filePath.c_str());
      }
      fontUpload.valid = false;
      fontUpload.buffer = {};
      LOG_DBG("WEB", "Font upload aborted");
      break;
    }
  }
}

void CrossPointWebServer::handleFontUpload() {
  if (fontUpload.valid) {
    FontInstaller installer(sdFontSystem.registry());
    installer.refreshRegistry();
    server->send(200, "application/json", "{\"ok\":true}");
    LOG_DBG("WEB", "Font upload complete: %s", fontUpload.filePath.c_str());
  } else {
    server->send(400, "application/json", "{\"error\":\"Invalid .cpfont file\"}");
  }
}

void CrossPointWebServer::handleFontDelete() {
  String body = server->arg("plain");
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);

  if (err || !doc["family"].is<const char*>()) {
    server->send(400, "application/json", "{\"error\":\"Invalid request\"}");
    return;
  }

  const char* familyName = doc["family"];
  FontInstaller installer(sdFontSystem.registry());
  auto result = installer.deleteFamily(familyName);

  if (result == FontInstaller::Error::OK) {
    installer.refreshRegistry();
    server->send(200, "application/json", "{\"ok\":true}");
    LOG_DBG("WEB", "Deleted font family: %s", familyName);
  } else {
    server->send(500, "application/json", "{\"error\":\"Delete failed\"}");
    LOG_ERR("WEB", "Failed to delete font family: %s", familyName);
  }
}

// ---- Wi-Fi Credentials API ----

void CrossPointWebServer::handleGetWifiNetworks() const {
  const auto& credentials = WIFI_STORE.getCredentials();
  const std::string& lastConnectedSsid = WIFI_STORE.getLastConnectedSsid();

  // Stream JSON array incrementally to avoid allocating the full response in memory
  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  ChunkedJsonArray out(server.get());

  char output[320];
  constexpr size_t outputSize = sizeof(output);
  JsonDocument doc;

  for (size_t i = 0; i < credentials.size(); i++) {
    doc.clear();
    doc["index"] = i;
    doc["ssid"] = credentials[i].ssid;
    // Never expose Wi-Fi passwords over the API — only indicate whether one is set
    doc["hasPassword"] = !credentials[i].password.empty();
    doc["isLastConnected"] = credentials[i].ssid == lastConnectedSsid;

    const size_t written = serializeJson(doc, output, outputSize);
    if (written >= outputSize) continue;

    out.addEntry(output, written);
  }

  out.finish();
  LOG_DBG("WEB", "Served Wi-Fi credentials API (%zu network(s))", credentials.size());
}

void CrossPointWebServer::handlePostWifiNetwork() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  std::string ssid = doc["ssid"] | std::string("");
  if (ssid.empty()) {
    server->send(400, "text/plain", "SSID is required");
    return;
  }

  // The password field is optional in the JSON payload. When absent (vs. present but empty),
  // preserve the existing password for updates. Empty passwords are valid for open networks.
  bool hasPasswordField = doc["password"].is<const char*>() || doc["password"].is<std::string>();
  std::string password = doc["password"] | std::string("");

  if (doc["index"].is<int>()) {
    int idx = doc["index"].as<int>();
    const auto& credentials = WIFI_STORE.getCredentials();
    if (idx < 0 || idx >= static_cast<int>(credentials.size())) {
      server->send(400, "text/plain", "Invalid network index");
      return;
    }

    const std::string oldSsid = credentials[static_cast<size_t>(idx)].ssid;
    if (!hasPasswordField) {
      password = credentials[static_cast<size_t>(idx)].password;
    }

    bool ok = true;
    if (oldSsid != ssid) {
      ok = WIFI_STORE.removeCredential(oldSsid) && WIFI_STORE.addCredential(ssid, password);
    } else {
      ok = WIFI_STORE.addCredential(ssid, password);
    }

    if (!ok) {
      server->send(400, "text/plain", "Failed to update Wi-Fi network");
      return;
    }

    LOG_DBG("WEB", "Updated Wi-Fi network at index %d (SSID: %s)", idx, ssid.c_str());
  } else {
    if (!WIFI_STORE.addCredential(ssid, password)) {
      server->send(400, "text/plain", "Cannot add network (limit reached)");
      return;
    }
    LOG_DBG("WEB", "Added Wi-Fi network: %s", ssid.c_str());
  }

  server->send(200, "text/plain", "OK");
}

// Uses POST (not HTTP DELETE) because ESP32 WebServer doesn't support DELETE with body.
void CrossPointWebServer::handleDeleteWifiNetwork() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  if (!doc["index"].is<int>()) {
    server->send(400, "text/plain", "Missing index");
    return;
  }

  int idx = doc["index"].as<int>();
  const auto& credentials = WIFI_STORE.getCredentials();
  if (idx < 0 || idx >= static_cast<int>(credentials.size())) {
    server->send(400, "text/plain", "Invalid network index");
    return;
  }

  const std::string ssid = credentials[static_cast<size_t>(idx)].ssid;
  if (!WIFI_STORE.removeCredential(ssid)) {
    server->send(400, "text/plain", "Failed to delete Wi-Fi network");
    return;
  }

  LOG_DBG("WEB", "Deleted Wi-Fi network at index %d (SSID: %s)", idx, ssid.c_str());
  server->send(200, "text/plain", "OK");
}

// ---- OPDS Server API ----

void CrossPointWebServer::handleGetOpdsServers() const {
  const auto& servers = OPDS_STORE.getServers();

  // Stream JSON array incrementally to avoid allocating the full response in memory
  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  ChunkedJsonArray out(server.get());

  char output[512];
  constexpr size_t outputSize = sizeof(output);
  JsonDocument doc;

  for (size_t i = 0; i < servers.size(); i++) {
    doc.clear();
    doc["index"] = i;
    doc["name"] = servers[i].name;
    doc["url"] = servers[i].url;
    doc["username"] = servers[i].username;
    // Never expose passwords over the API — only indicate whether one is set
    doc["hasPassword"] = !servers[i].password.empty();

    const size_t written = serializeJson(doc, output, outputSize);
    if (written >= outputSize) continue;

    out.addEntry(output, written);
  }

  out.finish();
  LOG_DBG("WEB", "Served OPDS servers API (%zu servers)", servers.size());
}

void CrossPointWebServer::handlePostOpdsServer() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  const std::string name = doc["name"] | std::string("");
  const std::string rawUrl = doc["url"] | std::string("");
  const std::string username = doc["username"] | std::string("");

  // The password field is optional in the JSON payload. When absent (vs. present but empty),
  // we preserve the existing password — the web UI omits it when the user hasn't changed it.
  bool hasPasswordField = doc["password"].is<const char*>() || doc["password"].is<std::string>();
  std::string password = doc["password"] | std::string("");

  const auto normalizedUrl = OpdsServerValidation::normalizeUrl(rawUrl);
  if (!normalizedUrl) {
    server->send(400, "text/plain", "Invalid URL");
    return;
  }
  if (name.size() > OpdsServerStore::MAX_NAME_LENGTH) {
    server->send(400, "text/plain", "Server name too long");
    return;
  }
  if (normalizedUrl->size() > OpdsServerStore::MAX_URL_LENGTH) {
    server->send(400, "text/plain", "URL too long");
    return;
  }
  if (username.size() > OpdsServerStore::MAX_USERNAME_LENGTH) {
    server->send(400, "text/plain", "Username too long");
    return;
  }

  OpdsServer opdsServer;
  opdsServer.name = name;
  opdsServer.url = *normalizedUrl;
  opdsServer.username = username;

  if (doc["index"].is<int>()) {
    int idx = doc["index"].as<int>();
    if (idx < 0 || idx >= static_cast<int>(OPDS_STORE.getCount())) {
      server->send(400, "text/plain", "Invalid server index");
      return;
    }
    // Preserve existing password if not explicitly provided
    if (!hasPasswordField) {
      const auto* existing = OPDS_STORE.getServer(static_cast<size_t>(idx));
      if (existing) password = existing->password;
    }
    if (password.size() > OpdsServerStore::MAX_PASSWORD_LENGTH) {
      server->send(400, "text/plain", "Password too long");
      return;
    }
    opdsServer.password = password;
    if (!OPDS_STORE.updateServer(static_cast<size_t>(idx), opdsServer)) {
      server->send(500, "text/plain", "Failed to save server");
      return;
    }
    LOG_DBG("WEB", "Updated OPDS server at index %d", idx);
  } else {
    if (OPDS_STORE.getCount() >= OpdsServerStore::MAX_SERVERS) {
      server->send(400, "text/plain", "Cannot add server (limit reached)");
      return;
    }
    if (password.size() > OpdsServerStore::MAX_PASSWORD_LENGTH) {
      server->send(400, "text/plain", "Password too long");
      return;
    }
    opdsServer.password = password;
    const auto insertedIndex = OPDS_STORE.addServer(opdsServer);
    if (!insertedIndex) {
      server->send(500, "text/plain", "Failed to save server");
      return;
    }
    LOG_DBG("WEB", "Added new OPDS server at index %zu: %s", *insertedIndex, opdsServer.name.c_str());
  }

  server->send(200, "text/plain", "OK");
}

// Uses POST (not HTTP DELETE) because ESP32 WebServer doesn't support DELETE with body.
void CrossPointWebServer::handleDeleteOpdsServer() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  if (!doc["index"].is<int>()) {
    server->send(400, "text/plain", "Missing index");
    return;
  }

  int idx = doc["index"].as<int>();
  if (idx < 0 || idx >= static_cast<int>(OPDS_STORE.getCount())) {
    server->send(400, "text/plain", "Invalid server index");
    return;
  }

  if (!OPDS_STORE.removeServer(static_cast<size_t>(idx))) {
    server->send(500, "text/plain", "Failed to delete server");
    return;
  }
  LOG_DBG("WEB", "Deleted OPDS server at index %d", idx);
  server->send(200, "text/plain", "OK");
}

// GET /api/plugins -> [{"name","title","mount","description","version","requires","allowedHosts",
//                       "root","disabled"}, ...]
// Only folders carrying a plugin.js are listed, since the pages' whole job is
// to load that file; manifest.json supplies the label, mount point and what the
// Plugins page shows about it. The pages skip a disabled plugin; the Plugins
// page lists it with the rest.
// Streamed rather than assembled: a JsonDocument holding every entry plus a
// manifest string each would be the largest allocation on this path.
void CrossPointWebServer::handlePluginList() const {
  if (rejectIfLowMemory(server.get())) return;

  // An entry with a description and a host list can pass 1 KB; on the heap, not the stack.
  constexpr size_t kEntryBytes = 1536;
  auto output = makeUniqueNoThrow<char[]>(kEntryBytes);
  if (!output) {
    server->send(500, "application/json", "{\"error\":\"OOM\"}");
    return;
  }

  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  ChunkedJsonArray out(server.get());

  JsonDocument doc;
  size_t listed = 0;

  for (const auto& e : PluginLocations::scanPlugins()) {
    if (!e.hasPluginJs) continue;

    doc.clear();
    doc["name"] = e.name;
    doc["title"] = e.name;      // manifest overrides below
    doc["mount"] = "settings";  // default page
    doc["root"] = e.root;
    doc["disabled"] = e.disabled;

    std::string manifest;
    if (e.hasManifest &&
        Storage.readFileToString("WEB", e.dir + "/manifest.json", PLUGIN_MANIFEST_MAX_BYTES, manifest)) {
      JsonDocument m;
      if (deserializeJson(m, manifest) == DeserializationError::Ok) {
        if (m["title"].is<const char*>()) doc["title"] = m["title"];
        if (m["mount"].is<const char*>()) doc["mount"] = m["mount"];
        if (m["description"].is<const char*>()) doc["description"] = m["description"];
        if (m["version"].is<const char*>()) doc["version"] = m["version"];
        // The page refuses a plugin whose required capabilities it lacks (missingCapabilities() in
        // PluginHost.js.inc), which it can only do if the list says what those are.
        if (m["requires"].is<JsonArrayConst>()) doc["requires"] = m["requires"];
        // Shown on the Plugins page, so who a plugin can reach is visible before it is used.
        if (m["allowedHosts"].is<JsonArrayConst>()) doc["allowedHosts"] = m["allowedHosts"];
      } else {
        LOG_DBG("WEB", "Plugin %s: unparseable manifest.json", e.name.c_str());
      }
    }

    const size_t written = serializeJson(doc, output.get(), kEntryBytes);
    if (written >= kEntryBytes) {
      LOG_DBG("WEB", "Plugin %s: entry too long, skipped", e.name.c_str());
      continue;
    }
    out.addEntry(output.get(), written);
    listed++;
  }

  out.finish();
  // Logged even when empty: "no plugins" and "the page never asked" look
  // identical from the outside otherwise.
  LOG_DBG("WEB", "Served plugin list (%u plugins)", static_cast<unsigned>(listed));
}

// GET /plugin?name=<plugin>&file=<file> -> that file from the plugin's folder.
// Both components are validated as single path segments, so a request cannot
// reach outside the folder findPluginDir resolved.
void CrossPointWebServer::handlePluginFile() const {
  // Streaming claims a 4KB chunk buffer, same as /download.
  if (rejectIfLowMemory(server.get())) return;

  const String name = server->arg("name");
  const String file = server->arg("file");
  if (!isSafePathComponent(name) || !isSafePathComponent(file)) {
    server->send(400, "text/plain", "Bad plugin path");
    return;
  }

  const std::string pluginDir = PluginLocations::findPluginDir(name.c_str());
  if (pluginDir.empty()) {
    LOG_DBG("WEB", "Plugin not found in any root: %s", name.c_str());
    server->send(404, "text/plain", "Plugin not found");
    return;
  }

  const std::string path = pluginDir + "/" + file.c_str();
  FsFile f = Storage.open(path.c_str());
  if (!f || !f.isOpen() || f.isDirectory()) {
    if (f) f.close();
    LOG_DBG("WEB", "Plugin file not found: %s", path.c_str());
    server->send(404, "text/plain", "File not found");
    return;
  }
  LOG_DBG("WEB", "Serving plugin file: %s (%u bytes)", path.c_str(), static_cast<unsigned>(f.size()));

  server->setContentLength(f.size());
  server->send(200, pluginContentType(file), "");

  NetworkClient client = server->client();
  const bool ok = HttpFileStreamer::streamFileToClient(f, client);
  client.clear();
  f.close();
  if (!ok) {
    LOG_DBG("WEB", "Plugin file streaming interrupted: %s/%s", name.c_str(), file.c_str());
  }
}

// GET /api/relay?plugin=<name>&url=<url>
//
// Fetches a URL the browser cannot reach itself. A page served from this device
// may not read a cross-origin response unless the remote sends CORS headers, and
// most do not; the device is not a browser and has no such restriction, so it
// fetches on the page's behalf and answers same-origin.
//
// That is a real capability, so it is fenced: GET only, and only to hosts the
// calling plugin declares in its own manifest. A redirect is followed only to a
// host the manifest allows too, and one elsewhere fails the request: otherwise an
// allowed host could 302 anywhere and the host actually fetched would never have
// been checked, which is how upstream's version could be bypassed. (Handing the
// 3xx back for the plugin to follow could not work: a page's fetch() follows
// redirects itself.)
//
// The body is streamed straight through rather than buffered: during a web
// session the heap has around 110KB free with a ~53KB largest block, and a
// single cover image can exceed that on its own.
void CrossPointWebServer::handleRelay() {
  if (rejectIfLowMemory(server.get())) return;

  const String plugin = server->arg("plugin");
  const String urlArg = server->arg("url");
  const std::string url = urlArg.c_str();

  if (!isSafePathComponent(plugin) || url.empty()) {
    server->send(400, "application/json", "{\"error\":\"missing plugin or url\"}");
    return;
  }
  // Only http(s): the client speaks nothing else, and this keeps the scheme out
  // of the allowlist's business.
  if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
    server->send(400, "application/json", "{\"error\":\"only http(s) urls\"}");
    return;
  }
  if (!relayHostAllowed(plugin, url)) {
    LOG_DBG("WEB", "Relay refused for %s: %s not in allowedHosts", plugin.c_str(), urlHost(url).c_str());
    server->send(403, "application/json", "{\"error\":\"host not allowed by plugin manifest\"}");
    return;
  }

  LOG_DBG("WEB", "Relay %s -> %s", plugin.c_str(), url.c_str());

  bool headerSent = false;
  bool overLimit = false;
  size_t total = 0;

  const bool ok = HttpDownloader::fetchUrl(
      url,
      [&](const uint8_t* data, size_t len) {
        if (total + len > MAX_RELAY_BYTES) {
          overLimit = true;
          return false;  // aborts the transfer
        }
        if (!headerSent) {
          // Content type is not forwarded - HttpDownloader does not surface response
          // headers - so the caller infers it from what it asked for.
          server->setContentLength(CONTENT_LENGTH_UNKNOWN);
          server->send(200, "application/octet-stream", "");
          headerSent = true;
        }
        HalSystem::feedWatchdog();
        server->sendContent(reinterpret_cast<const char*>(data), len);
        total += len;
        return true;
      },
      false, "", "", redirectsWithinAllowlist(plugin));

  if (!headerSent) {
    server->send(502, "application/json", "{\"error\":\"fetch failed\"}");
    return;
  }
  server->sendContent("");  // terminate the chunked response
  if (!ok || overLimit) {
    LOG_ERR("WEB", "Relay truncated after %u bytes%s: %s", static_cast<unsigned>(total),
            overLimit ? " (size limit)" : "", url.c_str());
  } else {
    LOG_DBG("WEB", "Relay served %u bytes", static_cast<unsigned>(total));
  }
}

// POST /api/fetch?plugin=<name>&url=<url>&dest=<sd path>
//
// Downloads straight to the card. Doing this through /api/relay plus /upload
// would move the file twice over WiFi and hold all of it in browser memory;
// for a cover that is merely wasteful, for a book it is prohibitive.
//
// Same fence as the relay - allowlisted per plugin manifest, redirects only within
// it - plus the write target must pass the same check /upload applies.
void CrossPointWebServer::handleFetchToSd() {
  if (rejectIfLowMemory(server.get())) return;

  const String plugin = server->arg("plugin");
  const std::string url = server->arg("url").c_str();
  const String dest = pluginWriteTarget(server->arg("dest"));

  if (!isSafePathComponent(plugin) || url.empty() || dest.isEmpty()) {
    server->send(400, "application/json", "{\"error\":\"missing plugin, url or dest\"}");
    return;
  }
  if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
    server->send(400, "application/json", "{\"error\":\"only http(s) urls\"}");
    return;
  }
  if (!relayHostAllowed(plugin, url)) {
    LOG_DBG("WEB", "Fetch refused for %s: %s not in allowedHosts", plugin.c_str(), urlHost(url).c_str());
    server->send(403, "application/json", "{\"error\":\"host not allowed by plugin manifest\"}");
    return;
  }

  LOG_DBG("WEB", "Fetch %s -> %s", url.c_str(), dest.c_str());
  const HttpDownloader::DownloadError result = HttpDownloader::downloadToFile(
      url, dest.c_str(), nullptr, "", "", HttpDownloader::TlsPolicy::Verified, redirectsWithinAllowlist(plugin));
  if (result != HttpDownloader::OK) {
    LOG_ERR("WEB", "Fetch to SD failed (%d): %s", static_cast<int>(result), url.c_str());
    server->send(502, "application/json", "{\"error\":\"download failed\"}");
    return;
  }

  // A downloaded book replaces whatever was cached for that path.
  clearBookCacheIfNeeded(dest);
  JsonDocument doc;
  doc["ok"] = true;
  doc["dest"] = dest.c_str();
  sendJson(server.get(), 200, doc);
}

// ---- Raw request bodies --------------------------------------------------------------------------

void CrossPointWebServer::RawFileUpload::start(const String& target, const size_t maxBytes) {
  if (file) file.close();  // left by a request the client dropped mid-body
  started = true;
  status = 0;
  error = "";
  written = 0;
  cap = maxBytes;
  path = target;
  temp = "";
  if (path.isEmpty()) {
    fail(400, "missing or invalid target");
    return;
  }
  temp = path + ".part";
  if (!Storage.openFileForWrite("WEB", temp.c_str(), file)) {
    temp = "";
    fail(500, "could not create file");
  }
}

void CrossPointWebServer::RawFileUpload::fail(const int httpStatus, const char* message) {
  status = httpStatus;
  error = message;
  if (file) file.close();
  if (!temp.isEmpty()) Storage.remove(temp.c_str());
}

void CrossPointWebServer::RawFileUpload::feed(const HTTPRaw& raw) {
  switch (raw.status) {
    case RAW_START:  // start() handles it, once the caller knows the target
      return;
    case RAW_WRITE:
      if (status != 0) return;
      if (written + raw.currentSize > cap) {
        fail(413, "file too large");
        return;
      }
      if (file.write(raw.buf, raw.currentSize) != raw.currentSize) {
        LOG_ERR("WEB", "raw upload short write at %u B to %s", static_cast<unsigned>(written), temp.c_str());
        fail(500, "short write");
        return;
      }
      written += raw.currentSize;
      HalSystem::feedWatchdog();
      return;
    case RAW_END:
      if (status != 0) return;
      file.close();
      if ((Storage.exists(path.c_str()) && !Storage.remove(path.c_str())) ||
          !Storage.rename(temp.c_str(), path.c_str())) {
        fail(500, "could not replace file");
      }
      return;
    case RAW_ABORTED:
      fail(400, "upload interrupted");
      return;
  }
}

bool CrossPointWebServer::RawFileUpload::answerFailure(WebServer& server) {
  if (!started) {
    // A multipart body is parsed as a form and never streams; every other content type does.
    server.send(415, "application/json", "{\"error\":\"send the file as the raw request body\"}");
    return true;
  }
  started = false;
  if (status == 0) return false;
  JsonDocument err;
  err["error"] = error;
  sendJson(&server, status, err);
  return true;
}

// POST /api/plugin-fs?plugin=<name>&path=<sd path>  (raw body = file contents)
//
// Writes one small file to the card. /upload already does this, so this exists
// for compatibility with plugins written against the upstream API rather than
// because the web UI needs it - hence the modest cap, which keeps it to the
// small-file job it advertises and leaves bulk transfers to /upload. The body streams through
// rawUpload, never held whole, so it needs no low-memory gate.
void CrossPointWebServer::handlePluginFs() {
  if (rawUpload.answerFailure(*server)) return;
  LOG_DBG("WEB", "plugin-fs wrote %u bytes to %s", static_cast<unsigned>(rawUpload.written), rawUpload.path.c_str());
  clearBookCacheIfNeeded(rawUpload.path);
  JsonDocument doc;
  doc["ok"] = true;
  doc["path"] = rawUpload.path.c_str();
  sendJson(server.get(), 200, doc);
}

void CrossPointWebServer::handlePluginFsData() {
  const HTTPRaw& raw = server->raw();
  if (raw.status == RAW_START) {
    tolerateUploadStalls(*server);
    const String plugin = server->arg("plugin");
    rawUpload.start(isSafePathComponent(plugin) ? pluginWriteTarget(server->arg("path")) : String(),
                    PLUGIN_FS_MAX_BYTES);
    return;
  }
  rawUpload.feed(raw);
}

// ---- The Plugins page ----------------------------------------------------------------------------
//
// The browser does the fetching and unpacking (from a .zip the user picked, or from the catalog on
// GitHub, which allows cross-origin reads); the device only stores what it is sent. An install goes
// into a staging folder file by file and replaces the plugin in one rename, so a half-sent plugin
// never runs.

namespace {

constexpr size_t PLUGIN_FILE_MAX_BYTES = 512u * 1024u;
constexpr char PLUGIN_DISABLED_MARKER[] = "disabled";

std::string pluginInstallDir(const String& name) {
  return std::string(PluginLocations::kInstallRoot) + "/" + name.c_str();
}

std::string pluginStagingDir(const String& name) { return pluginInstallDir(name) + ".new"; }

}  // namespace

void CrossPointWebServer::handlePluginsPage() const { serveBundlePage(web_bundle::PluginsPageHtml); }

// GET /api/book/opf?path=<epub> -> the OPF package document from inside the book, as it is.
//
// What the Details dialog reads a book's own metadata from when no sidecar overrides it. The dialog
// used to download the whole EPUB, often tens of MB over the device's one connection, to read a few
// KB of it in the browser.
void CrossPointWebServer::handleBookOpf() const {
  if (rejectIfLowMemory(server.get())) return;
  const String path = normalizeWebPath(server->arg("path"));
  if (!FsHelpers::hasEpubExtension(path) || isProtectedWebPath(path, Target::Item, hiddenAllowed()) ||
      !Storage.exists(path.c_str())) {
    server->send(404, "application/json", "{\"error\":\"no such EPUB\"}");
    return;
  }
  const Epub epub(path.c_str(), "/.crosspoint");
  std::string opfPath;
  if (!epub.contentOpfPath(&opfPath)) {
    server->send(422, "application/json", "{\"error\":\"no OPF in this EPUB\"}");
    return;
  }
  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/oebps-package+xml", "");
  ChunkedResponse out(server.get());
  ChunkedPrint body(out);
  // A failure from here on reaches the page as truncated XML, which it treats as unreadable and
  // falls back to downloading the book.
  if (!epub.readItemContentsToStream(opfPath, body, 1024)) LOG_ERR("WEB", "Reading the OPF of %s failed", path.c_str());
  out.finish();
}

// POST /api/book/replace?path=<book.epub>[&keep=1]  (raw body = the new book)
//
// The File Manager's in-place optimize: it downloads a book, optimizes it in the browser and sends
// it back here. The new file is written beside the book and swapped in only once complete. The
// original stays as <book>.bak - unless a .bak is already there, which then holds the older
// original and is kept. keep=1 means the optimizer left the chapter list as it was: the reading
// position and the bookmarks stay, and only what the new content makes wrong (laid-out chapters,
// indexes, thumbnails) goes. Without it the book starts afresh, as after any re-upload.

namespace {

// A book of any size the card holds; the browser cannot send what it could not optimize anyway.
constexpr size_t BOOK_REPLACE_MAX_BYTES = 512u * 1024u * 1024u;

// The target /api/book/replace accepts: an existing EPUB the web interface may write.
String bookReplaceTarget(const String& rawPath) {
  const String path = pluginWriteTarget(rawPath);
  if (path.isEmpty() || !FsHelpers::hasEpubExtension(path) || !Storage.exists(path.c_str())) return "";
  return path;
}

// Everything in the book's cache folder except the reading position and the bookmarks.
void dropLayoutCache(const String& bookPath) {
  const std::string cacheDir = Epub(bookPath.c_str(), "/.crosspoint").getCachePath();
  FsFile dir = Storage.open(cacheDir.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }
  char name[64];
  for (FsFile entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    HalSystem::feedWatchdog();
    entry.getName(name, sizeof(name));
    const bool isDir = entry.isDirectory();
    entry.close();
    if (strcmp(name, "progress.bin") == 0 || strcmp(name, "bookmarks.bin") == 0) continue;
    const std::string child = cacheDir + "/" + name;
    if (isDir ? !Storage.removeDir(child.c_str()) : !Storage.remove(child.c_str())) {
      LOG_ERR("WEB", "Could not remove %s", child.c_str());
    }
  }
  dir.close();
}

}  // namespace

void CrossPointWebServer::handleBookReplace() {
  const String path = bookReplacePath;  // validated in handleBookReplaceData(); rawUpload.path is <path>.new
  bookReplacePath = "";
  if (rawUpload.answerFailure(*server)) return;
  const String fresh = path + ".new";
  const String backup = path + ".bak";
  // The upload wrote <path>.new.part and renamed it to <path>.new; now <path> itself moves.
  const bool keepOlderBackup = Storage.exists(backup.c_str());
  const bool movedAside = keepOlderBackup ? Storage.remove(path.c_str()) : Storage.rename(path.c_str(), backup.c_str());
  if (!movedAside) {
    Storage.remove(fresh.c_str());
    server->send(500, "application/json", "{\"error\":\"could not set the original aside\"}");
    return;
  }
  if (!Storage.rename(fresh.c_str(), path.c_str())) {
    LOG_ERR("WEB", "Could not swap in %s", fresh.c_str());
    if (!keepOlderBackup) Storage.rename(backup.c_str(), path.c_str());
    server->send(500, "application/json", "{\"error\":\"could not replace the book\"}");
    return;
  }

  const bool keep = server->arg("keep") == "1";
  if (keep) {
    dropLayoutCache(path);
  } else {
    clearBookCacheIfNeeded(path);
    GLOBAL_BOOKMARKS.removeBySourcePath(path.c_str());
  }
  LOG_DBG("WEB", "Replaced %s (%u B, %s)", path.c_str(), static_cast<unsigned>(rawUpload.written),
          keep ? "reading state kept" : "reading state reset");

  JsonDocument doc;
  doc["ok"] = true;
  doc["path"] = path.c_str();
  doc["backup"] = backup.c_str();
  doc["keptOlderBackup"] = keepOlderBackup;
  doc["keptReadingState"] = keep;
  sendJson(server.get(), 200, doc);
}

void CrossPointWebServer::handleBookReplaceData() {
  const HTTPRaw& raw = server->raw();
  if (raw.status == RAW_START) {
    tolerateUploadStalls(*server);
    const String target = bookReplaceTarget(server->arg("path"));
    bookReplacePath = target;
    rawUpload.start(target.isEmpty() ? target : target + ".new", BOOK_REPLACE_MAX_BYTES);
    return;
  }
  rawUpload.feed(raw);
}

void CrossPointWebServer::handlePluginInstallBegin() const {
  const String name = server->arg("name");
  if (!isSafePathComponent(name)) {
    server->send(400, "application/json", "{\"error\":\"invalid plugin name\"}");
    return;
  }
  const std::string staging = pluginStagingDir(name);
  Storage.mkdir(PluginLocations::kInstallRoot);
  if (Storage.exists(staging.c_str())) Storage.removeDir(staging.c_str());  // left by a cut install
  if (!Storage.mkdir(staging.c_str())) {
    server->send(500, "application/json", "{\"error\":\"could not create the staging folder\"}");
    return;
  }
  server->send(200, "application/json", "{\"ok\":true}");
}

void CrossPointWebServer::handlePluginInstallFile() {
  if (rawUpload.answerFailure(*server)) return;
  LOG_DBG("WEB", "plugin file %s (%u B)", rawUpload.path.c_str(), static_cast<unsigned>(rawUpload.written));
  server->send(200, "application/json", "{\"ok\":true}");
}

void CrossPointWebServer::handlePluginInstallFileData() {
  const HTTPRaw& raw = server->raw();
  if (raw.status == RAW_START) {
    tolerateUploadStalls(*server);
    // Flat files only: /plugin serves single path components, so a nested one could never load.
    const String name = server->arg("name");
    const String fileName = server->arg("file");
    const std::string staging = pluginStagingDir(name);
    const bool valid = isSafePathComponent(name) && isSafePathComponent(fileName) && Storage.exists(staging.c_str());
    rawUpload.start(valid ? String((staging + "/" + fileName.c_str()).c_str()) : String(), PLUGIN_FILE_MAX_BYTES);
    return;
  }
  rawUpload.feed(raw);
}

void CrossPointWebServer::handlePluginInstallCommit() const {
  const String name = server->arg("name");
  if (!isSafePathComponent(name)) {
    server->send(400, "application/json", "{\"error\":\"invalid plugin name\"}");
    return;
  }
  const std::string staging = pluginStagingDir(name);
  if (!Storage.exists((staging + "/plugin.js").c_str())) {
    server->send(400, "application/json", "{\"error\":\"no plugin.js was sent\"}");
    return;
  }
  const std::string live = pluginInstallDir(name);
  if ((Storage.exists(live.c_str()) && !Storage.removeDir(live.c_str())) ||
      !Storage.rename(staging.c_str(), live.c_str())) {
    server->send(500, "application/json", "{\"error\":\"could not replace the plugin\"}");
    return;
  }
  LOG_INF("WEB", "Plugin %s installed", name.c_str());
  server->send(200, "application/json", "{\"ok\":true}");
}

// Removes the plugin from every root, so a copy in another root does not take its place.
void CrossPointWebServer::handlePluginRemove() const {
  const String name = server->arg("name");
  if (!isSafePathComponent(name)) {
    server->send(400, "application/json", "{\"error\":\"invalid plugin name\"}");
    return;
  }
  bool removed = false;
  for (const char* root : PluginLocations::kRoots) {
    const std::string dir = std::string(root) + "/" + name.c_str();
    if (!Storage.exists(dir.c_str())) continue;
    if (!Storage.removeDir(dir.c_str())) {
      server->send(500, "application/json", "{\"error\":\"could not remove the plugin\"}");
      return;
    }
    removed = true;
  }
  if (!removed) {
    server->send(404, "application/json", "{\"error\":\"no such plugin\"}");
    return;
  }
  LOG_INF("WEB", "Plugin %s removed", name.c_str());
  server->send(200, "application/json", "{\"ok\":true}");
}

// A disabled plugin keeps its folder and carries a "disabled" marker, which /api/plugins reports and
// the pages honour: kept on the card rather than in one browser, so it holds for every browser.
void CrossPointWebServer::handlePluginEnable() const {
  const String name = server->arg("name");
  if (!isSafePathComponent(name)) {
    server->send(400, "application/json", "{\"error\":\"invalid plugin name\"}");
    return;
  }
  const std::string dir = PluginLocations::findPluginDir(name.c_str());
  if (dir.empty()) {
    server->send(404, "application/json", "{\"error\":\"no such plugin\"}");
    return;
  }
  const std::string marker = dir + "/" + PLUGIN_DISABLED_MARKER;
  const bool enable = server->arg("on") != "0";
  bool ok = true;
  if (enable) {
    if (Storage.exists(marker.c_str())) ok = Storage.remove(marker.c_str());
  } else if (!Storage.exists(marker.c_str())) {
    FsFile f;
    ok = Storage.openFileForWrite("WEB", marker.c_str(), f);
    if (ok) f.close();
  }
  if (!ok) {
    server->send(500, "application/json", "{\"error\":\"could not change the plugin\"}");
    return;
  }
  server->send(200, "application/json", "{\"ok\":true}");
}

// WebSocket callback trampoline
void CrossPointWebServer::wsEventCallback(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  if (wsInstance) {
    wsInstance->onWebSocketEvent(num, type, payload, length);
  }
}

// WebSocket event handler for fast binary uploads
// Protocol:
//   1. Client sends TEXT message: "START:<filename>:<size>:<path>"
//   2. Client sends BINARY messages with file data chunks
//   3. Server sends TEXT "PROGRESS:<received>:<total>" after each chunk
//   4. Server sends TEXT "DONE" or "ERROR:<message>" when complete
void CrossPointWebServer::onWebSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      LOG_DBG("WS", "Client %u disconnected", num);
      // Only clean up if this is the client that owns the active upload.
      // A new client may have already started a fresh upload before this
      // DISCONNECTED event fires (race condition on quick cancel + retry).
      if (num == wsUploadClientNum && wsUploadInProgress && wsUploadFile) {
        abortWsUpload("WS");
      }
      break;

    case WStype_CONNECTED: {
      LOG_DBG("WS", "Client %u connected", num);
      break;
    }

    case WStype_TEXT: {
      // Parse control messages
      String msg = String(reinterpret_cast<const char*>(payload));
      LOG_DBG("WS", "Text from client %u: %s", num, msg.c_str());

      if (msg.startsWith("START:")) {
        // Reject any START while an upload is already active to prevent
        // leaking the open wsUploadFile handle (owning client re-START included)
        if (wsUploadInProgress) {
          wsServer->sendTXT(num, "ERROR:Upload already in progress");
          break;
        }

        // Parse: START:<filename>:<size>:<path>
        int firstColon = msg.indexOf(':', 6);
        int secondColon = msg.indexOf(':', firstColon + 1);

        if (firstColon > 0 && secondColon > 0) {
          wsUploadFileName = msg.substring(6, firstColon);
          String sizeToken = msg.substring(firstColon + 1, secondColon);
          bool sizeValid = sizeToken.length() > 0;
          int digitStart = (sizeValid && sizeToken[0] == '+') ? 1 : 0;
          if (digitStart > 0 && sizeToken.length() < 2) sizeValid = false;
          for (int i = digitStart; i < (int)sizeToken.length() && sizeValid; i++) {
            if (!isdigit((unsigned char)sizeToken[i])) sizeValid = false;
          }
          if (!sizeValid) {
            LOG_DBG("WS", "START rejected: invalid size token '%s'", sizeToken.c_str());
            wsServer->sendTXT(num, "ERROR:Invalid START format");
            return;
          }
          wsUploadSize = sizeToken.toInt();
          wsUploadPath = msg.substring(secondColon + 1);
          wsUploadReceived = 0;
          wsLastProgressSent = 0;
          wsUploadStartTime = millis();

          // Same containment rule as the HTTP upload above. This path is ours, not
          // upstream's -- they have no WebSocket upload -- but the exposure is
          // identical: both the directory and the client-supplied file name are
          // concatenated into a write target.
          if (!isSafePathComponent(wsUploadFileName)) {
            LOG_DBG("WS", "START rejected: unsafe file name '%s'", wsUploadFileName.c_str());
            wsServer->sendTXT(num, "ERROR:Invalid file name");
            return;
          }
          wsUploadPath = normalizeWebPath(wsUploadPath);

          // Build file path
          String filePath = wsUploadPath;
          if (!filePath.endsWith("/")) filePath += "/";
          filePath += wsUploadFileName;

          // Before the overwrite below removes anything; same rule as the HTTP upload.
          if (isProtectedWebPath(wsUploadPath, Target::Directory, hiddenAllowed()) ||
              isProtectedWebPath(filePath, Target::Item, hiddenAllowed())) {
            LOG_DBG("WS", "START rejected: protected target '%s'", filePath.c_str());
            wsServer->sendTXT(num, "ERROR:Cannot write to a protected location");
            return;
          }

          LOG_DBG("WS", "Starting upload: %s (%d bytes) to %s", wsUploadFileName.c_str(), wsUploadSize,
                  filePath.c_str());

          // Check if file exists and remove it
          HalSystem::feedWatchdog();
          if (Storage.exists(filePath.c_str())) {
            Storage.remove(filePath.c_str());
          }

          // Open file for writing
          HalSystem::feedWatchdog();
          if (!Storage.openFileForWrite("WS", filePath, wsUploadFile)) {
            wsServer->sendTXT(num, "ERROR:Failed to create file");
            wsUploadInProgress = false;
            wsUploadClientNum = 255;
            return;
          }
          HalSystem::feedWatchdog();

          // Zero-byte upload: complete immediately without waiting for BIN frames
          if (wsUploadSize == 0) {
            wsUploadFile.close();
            wsLastCompleteName = wsUploadFileName;
            wsLastCompleteSize = 0;
            wsLastCompleteAt = millis();
            LOG_DBG("WS", "Zero-byte upload complete: %s", filePath.c_str());
            clearBookCacheIfNeeded(filePath);
            wsServer->sendTXT(num, "DONE");
            wsLastProgressSent = 0;
            break;
          }

          wsUploadClientNum = num;
          wsUploadInProgress = true;
          wsServer->sendTXT(num, "READY");
        } else {
          wsServer->sendTXT(num, "ERROR:Invalid START format");
        }
      }
      break;
    }

    case WStype_BIN: {
      if (!wsUploadInProgress || !wsUploadFile || num != wsUploadClientNum) {
        wsServer->sendTXT(num, "ERROR:No upload in progress");
        return;
      }

      // Write binary data directly to file
      size_t remaining = wsUploadSize - wsUploadReceived;
      if (length > remaining) {
        abortWsUpload("WS");
        wsServer->sendTXT(num, "ERROR:Upload overflow");
        return;
      }
      HalSystem::feedWatchdog();
      size_t written = wsUploadFile.write(payload, length);
      HalSystem::feedWatchdog();

      if (written != length) {
        abortWsUpload("WS");
        wsServer->sendTXT(num, "ERROR:Write failed - disk full?");
        return;
      }

      wsUploadReceived += written;

      // Send progress update (every 64KB or at end)
      if (wsUploadReceived - wsLastProgressSent >= 65536 || wsUploadReceived >= wsUploadSize) {
        String progress = "PROGRESS:" + String(wsUploadReceived) + ":" + String(wsUploadSize);
        wsServer->sendTXT(num, progress);
        wsLastProgressSent = wsUploadReceived;
      }

      // Check if upload complete
      if (wsUploadReceived >= wsUploadSize) {
        wsUploadFile.close();
        wsUploadInProgress = false;
        wsUploadClientNum = 255;

        wsLastCompleteName = wsUploadFileName;
        wsLastCompleteSize = wsUploadSize;
        wsLastCompleteAt = millis();

        unsigned long elapsed = millis() - wsUploadStartTime;
        float kbps = (elapsed > 0) ? (wsUploadSize / 1024.0) / (elapsed / 1000.0) : 0;

        LOG_DBG("WS", "Upload complete: %s (%d bytes in %lu ms, %.1f KB/s)", wsUploadFileName.c_str(), wsUploadSize,
                elapsed, kbps);

        // Clear epub cache to prevent stale metadata issues when overwriting files
        String filePath = wsUploadPath;
        if (!filePath.endsWith("/")) filePath += "/";
        filePath += wsUploadFileName;
        clearBookCacheIfNeeded(filePath);

        wsServer->sendTXT(num, "DONE");
        wsLastProgressSent = 0;
      }
      break;
    }

    default:
      break;
  }
}
