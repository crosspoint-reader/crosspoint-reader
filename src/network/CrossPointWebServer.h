#pragma once

#include <HalStorage.h>
#include <NetworkUdp.h>
#include <WebServer.h>
#include <WebSocketsServer.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "html/WebBundle.generated.h"

// Structure to hold file information
struct FileInfo {
  String name;
  size_t size;
  bool isEpub;
  bool isDirectory;
};

class CrossPointWebServer {
 public:
  struct WsUploadStatus {
    bool inProgress = false;
    size_t received = 0;
    size_t total = 0;
    std::string filename;
    std::string lastCompleteName;
    size_t lastCompleteSize = 0;
    unsigned long lastCompleteAt = 0;
    // Last successful /delete request (HTTP, not WebSocket): name of the last
    // item removed, how many items that request removed, and when.
    std::string lastDeleteName;
    size_t lastDeleteCount = 0;
    unsigned long lastDeleteAt = 0;
  };

  // Used by POST upload handler
  struct UploadState {
    FsFile file;
    String fileName;
    String path = "/";
    size_t size = 0;
    bool success = false;
    String error = "";

    // Upload write buffer - batches small writes into larger SD card operations
    // 4KB is a good balance: large enough to reduce syscall overhead, small enough
    // to keep individual write times short and avoid watchdog issues
    static constexpr size_t UPLOAD_BUFFER_SIZE = 4096;  // 4KB buffer
    std::vector<uint8_t> buffer;
    size_t bufferPos = 0;

    // For the log and the failure report: the request body's length (the file plus its form
    // framing), when data last arrived, and whether Back on the device ended it.
    size_t expected = 0;
    unsigned long lastDataAt = 0;
    bool cancelled = false;

    // The last upload that broke off, until the next one starts. WebServer drops such a request
    // without answering it, so the browser sees only a broken connection; /api/web/status hands it
    // this instead, for the setup page to explain.
    struct Failure {
      bool set = false;
      String name;
      size_t received = 0;
      size_t expected = 0;
      unsigned long ms = 0;      // from the start to the end
      unsigned long idleMs = 0;  // since data last arrived
      const char* reason = "";   // "stalled", "closed" or "cancelled"
      int rssi = 0;              // 0 in hotspot mode
    } failure;

    UploadState() = default;
  } upload;

  CrossPointWebServer();
  ~CrossPointWebServer();

  // Start the web server (call after WiFi is connected)
  void begin();

  // Stop the web server
  void stop();

  // Call this periodically to handle client requests
  void handleClient();

  // Check if server is running
  bool isRunning() const { return running; }

  WsUploadStatus getWsUploadStatus() const;

  // A multipart HTTP upload (/upload, /api/fonts/upload) is read whole inside ONE handleClient()
  // call, so the caller's loop is blind until it ends. This is polled on each received chunk
  // instead; returning true drops the client, and the upload's ABORTED path deletes the partial
  // file. Runs on the task that calls handleClient(). WebSocket uploads return between frames and
  // do not use it.
  void setUploadCancelCheck(std::function<bool()> check) { uploadCancelCheck = std::move(check); }

  // Get the port number
  uint16_t getPort() const { return port; }

 private:
  std::unique_ptr<WebServer> server = nullptr;
  std::unique_ptr<WebSocketsServer> wsServer = nullptr;
  bool running = false;
  bool apMode = false;  // true when running in AP mode, false for STA mode
  uint16_t port = 80;
  uint16_t wsPort = 81;  // WebSocket port
  NetworkUDP udp;
  bool udpActive = false;
  std::function<bool()> uploadCancelCheck;
  bool dropUploadIfCancelled() const;

  // WebSocket upload state
  void onWebSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length);
  static void wsEventCallback(uint8_t num, WStype_t type, uint8_t* payload, size_t length);
  void abortWsUpload(const char* tag);

  // File scanning
  // Plain function pointer + context rather than std::function: the callback runs once per
  // directory entry, and a std::function taking FileInfo by value copies its String each time.
  using FileVisitor = void (*)(const FileInfo& info, void* context);
  void scanFiles(const char* path, FileVisitor visitor, void* context) const;
  bool isEpubFile(const String& filename) const;

  // Request handlers
  void serveBundlePage(web_bundle::Index page) const;
  void handleRoot() const;
  void handleWelcomePage() const;
  void handleWebSetupPage() const;
  void handleWebStatus() const;
  void handleWebInstall() const;
  void handleSystemInfoPage() const;
  void handleDiagnosticsPage() const;
  void handleBootDiagnostics() const;
  void handleBundledScript() const;  // GET /js/<name>: a script from the SD-card bundle
  void handleNotFound() const;
  void handleStatus() const;
  void handleStatusFast() const;
  void handleFileListData() const;
  void handleDownload() const;
  void handleUpload(UploadState& state) const;
  void handleUploadPost(UploadState& state) const;
  void handleCreateFolder() const;
  void handleRename() const;
  void handleMove() const;
  void handleDelete() const;

  // Settings handlers
  void handleSettingsPage() const;
  void handleGetSettings() const;
  void handlePostSettings();

  // Font management handlers
  void handleFontsPage() const;
  void handleFontList();
  void handleFontManifest();
  void handleFontDownload();
  void handleFontUpload();
  void handleFontUploadData();
  void handleFontDelete();

  void handleDictionaryPage() const;

  struct FontUploadState {
    FsFile file;
    std::string familyName;
    std::string filePath;
    bool valid = false;
    bool magicChecked = false;
    uint8_t header[8] = {0};
    size_t headerBytesReceived = 0;
    size_t bytesWritten = 0;
    static constexpr size_t BUFFER_SIZE = 4096;
    std::vector<uint8_t> buffer;
    size_t bufferPos = 0;

    FontUploadState() = default;
  } fontUpload;

  // OPDS server handlers
  void handleGetOpdsServers() const;
  void handlePostOpdsServer();
  void handleDeleteOpdsServer();

  // Wi-Fi credential handlers
  void handleGetWifiNetworks() const;
  void handlePostWifiNetwork();
  void handleDeleteWifiNetwork();

  // Reading-stats handlers
  void handleStatsPage() const;
  void handleStatsApi() const;
  void handleStatsExport() const;
  void handleStatsRemove() const;

  // Web-UI plugins: JS on the SD card that the Settings and File Manager pages
  // discover and load, so the web interface can be extended without a firmware
  // build. The firmware only lists the folders and serves their files - plugin
  // code runs in the browser, never on the device, and reaches the card through
  // the same endpoints the pages themselves use.
  //
  // Both endpoints are ported from crosspoint-reader PR #2734 ("feat: Add
  // browser-side plugin system with SD card support", Justin Mitchell /
  // @itsthisjustin). His /api/plugins and /plugin contract is preserved so
  // plugins written for either firmware work on both; the implementations
  // differ (streamed listing, low-memory guard, tighter manifest cap). None of
  // that branch's device-capability endpoints are ported.
  void handlePluginList() const;  // GET /api/plugins -> discovered plugins
  void handlePluginFile() const;  // GET /plugin?name&file -> one file from its folder
  // GET /api/relay?plugin&url -> fetch a URL the browser cannot reach itself.
  // GET only, allowlisted per plugin manifest, no redirects, streamed not
  // buffered. See the definition for why each of those is load-bearing.
  void handleRelay();
  // POST /api/fetch?plugin&url&dest -> download straight to the card, avoiding
  // the double transfer of relaying through the browser and uploading back.
  void handleFetchToSd();
  // POST /api/plugin-fs?plugin&path -> write one small file. /upload already
  // covers this; kept for compatibility with upstream-written plugins.
  void handlePluginFs();
  void handlePluginFsData();

  // The Plugins page (/plugins): install, remove, enable and disable. Plugins installed here go to
  // /.crosspoint/plugins, which the web file endpoints leave alone unless Show Hidden Files is on;
  // one copied by hand into /plugins or /.plugins is listed and managed the same way.
  void handlePluginsPage() const;
  // The File Manager's Details dialog reads an EPUB's own OPF from here.
  void handleBookOpf() const;
  // POST /api/book/replace?path&keep (raw body): swaps in an optimized copy of a book, keeping the
  // original as <book>.bak.
  void handleBookReplace();
  void handleBookReplaceData();
  void handlePluginInstallBegin() const;  // POST /api/plugins/begin?name -> fresh staging folder
  void handlePluginInstallFile();         // POST /api/plugins/file?name&file (raw body) -> one file
  void handlePluginInstallFileData();
  void handlePluginInstallCommit() const;  // POST /api/plugins/commit?name -> staging replaces the plugin
  void handlePluginRemove() const;         // POST /api/plugins/remove?name
  void handlePluginEnable() const;         // POST /api/plugins/enable?name&on=0|1

  // A raw request body streamed to a file. Raw, so binary-safe: the framework's buffered "plain"
  // argument is built with String(char*) and ends at the first NUL byte. Written to <path>.part,
  // capped as it arrives, and renamed over <path> only once complete, so a body that turns out too
  // large, or a client that goes away, never replaces a file that was there. One instance serves
  // every such route: the server handles one request at a time.
  struct RawFileUpload {
    FsFile file;
    String path;  // the validated target
    String temp;  // written here, renamed over `path` once complete
    size_t cap = 0;
    size_t written = 0;
    bool started = false;    // a raw body arrived
    int status = 0;          // 0 while all is well, else the HTTP status to answer with
    const char* error = "";  // its message

    // RAW_START: `target` is the validated path, or empty when the request named none it may write.
    void start(const String& target, size_t maxBytes);
    // RAW_WRITE, RAW_END and RAW_ABORTED.
    void feed(const HTTPRaw& raw);
    void fail(int httpStatus, const char* message);
    // The response once the body is in: false (and an error sent) when it failed or never streamed.
    bool answerFailure(WebServer& server);
  } rawUpload;
  String bookReplacePath;  // the book /api/book/replace is replacing, set at the start of its body
};
