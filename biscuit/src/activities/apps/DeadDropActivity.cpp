#include "DeadDropActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_random.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/RadioManager.h"

// Minimal HTML page embedded in flash
static const char DROP_HTML[] PROGMEM =
    "<!DOCTYPE html><html><head><meta name=\"viewport\" content=\"width=device-width\">"
    "<title>Dead Drop</title>"
    "<style>body{font-family:sans-serif;max-width:600px;margin:0 auto;padding:16px}"
    "h1{font-size:1.5em}input[type=file]{margin:8px 0}button{padding:8px 16px}"
    ".files a{display:block;padding:4px 0}</style></head>"
    "<body><h1>Dead Drop</h1>"
    "<form method=\"POST\" action=\"/upload\" enctype=\"multipart/form-data\">"
    "<input type=\"file\" name=\"file\"><button type=\"submit\">Upload</button></form>"
    "<h2>Files</h2><div class=\"files\" id=\"fl\">Loading...</div>"
    "<script>fetch('/list').then(r=>r.json()).then(f=>{"
    "let h='';f.forEach(n=>{h+='<a href=\"/download?file='+encodeURIComponent(n)+'\">'+"
    "n+'</a>'});"
    "document.getElementById('fl').innerHTML=h||'No files yet'})</script>"
    "</body></html>";

// Static instance pointer for web handler lambdas
DeadDropActivity* DeadDropActivity::activeInstance = nullptr;

// constexpr array definitions (required for ODR)
constexpr int DeadDropActivity::TIMER_OPTIONS[];

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void DeadDropActivity::onEnter() {
  Activity::onEnter();
  state = CONFIG;
  configIndex = 0;
  timerIndex = 1;
  timerMinutes = TIMER_OPTIONS[timerIndex];
  filesReceived = 0;
  connectedClients = 0;
  ssidName.clear();
  requestUpdate();
}

void DeadDropActivity::onExit() {
  Activity::onExit();
  if (state == ACTIVE) {
    stopDrop();
  }
}

// ---------------------------------------------------------------------------
// startDrop / stopDrop
// ---------------------------------------------------------------------------

void DeadDropActivity::startDrop() {
  Storage.mkdir("/biscuit");
  Storage.mkdir(DROP_DIR);

  // Generate random SSID suffix
  char suffix[5];
  snprintf(suffix, sizeof(suffix), "%04X", static_cast<uint16_t>(esp_random() & 0xFFFF));
  ssidName = std::string("DROP-") + suffix;

  RADIO.ensureWifi();
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssidName.c_str());

  setupWebServer();

  startTime = millis();
  filesReceived = 0;
  connectedClients = 0;
  state = ACTIVE;
  activeInstance = this;
}

void DeadDropActivity::stopDrop() {
  if (server) {
    server->stop();
    delete server;
    server = nullptr;
  }
  // Close any file left open from an interrupted upload
  if (uploadInProgress) {
    uploadFile.close();
    uploadInProgress = false;
  }
  WiFi.softAPdisconnect(true);
  RADIO.shutdown();
  activeInstance = nullptr;
}

// ---------------------------------------------------------------------------
// Web server setup
// ---------------------------------------------------------------------------

void DeadDropActivity::setupWebServer() {
  server = new WebServer(80);

  server->on("/", HTTP_GET, [this] { handleRoot(); });

  // Upload: second lambda is the upload data handler, first is called on completion
  server->on(
      "/upload", HTTP_POST, [this] { handleUpload(); }, [this] { handleUploadData(); });

  server->on("/list", HTTP_GET, [this] { handleFileList(); });
  server->on("/download", HTTP_GET, [this] { handleDownload(); });

  server->begin();
  WiFi.setSleep(false);
}

// ---------------------------------------------------------------------------
// Web handlers
// ---------------------------------------------------------------------------

void DeadDropActivity::handleRoot() {
  if (!server) return;
  // Send from PROGMEM — cast away const for send() which takes char*
  server->send_P(200, "text/html", DROP_HTML);
}

void DeadDropActivity::handleUploadData() {
  if (!server) return;

  HTTPUpload& upload = server->upload();

  if (upload.status == UPLOAD_FILE_START) {
    // Sanitize filename: strip any path separators
    String fname = upload.filename;
    int lastSlash = fname.lastIndexOf('/');
    if (lastSlash >= 0) fname = fname.substring(lastSlash + 1);
    if (fname.isEmpty()) fname = "unnamed";

    String filePath = DROP_DIR + fname;

    // Remove existing file if present
    if (Storage.exists(filePath.c_str())) {
      Storage.remove(filePath.c_str());
    }

    uploadFile = Storage.open(filePath.c_str(), O_WRITE | O_CREAT | O_TRUNC);
    uploadInProgress = uploadFile ? true : false;

    LOG_DBG("DEAD", "Upload start: %s (open=%d)", filePath.c_str(), (int)uploadInProgress);

  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadInProgress && uploadFile) {
      uploadFile.write(upload.buf, upload.currentSize);
    }

  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadInProgress && uploadFile) {
      uploadFile.close();
      uploadInProgress = false;
      filesReceived++;
      LOG_DBG("DEAD", "Upload complete, total=%d", filesReceived);
    }
  }
}

void DeadDropActivity::handleUpload() {
  if (!server) return;
  server->send(200, "text/plain", "OK");
}

void DeadDropActivity::handleFileList() {
  if (!server) return;

  // Build a JSON array of filenames in DROP_DIR
  // Use a fixed-size buffer to avoid heap fragmentation — 2KB is ample for ~20 filenames
  char buf[2048];
  int pos = 0;
  buf[pos++] = '[';

  HalFile dir = Storage.open(DROP_DIR, O_RDONLY);
  if (dir) {
    bool first = true;
    HalFile entry;
    while ((entry = dir.openNextFile())) {
      if (!entry.isDirectory()) {
        char name[256];
        entry.getName(name, sizeof(name));
        if (!first) {
          if (pos < static_cast<int>(sizeof(buf)) - 4) buf[pos++] = ',';
        }
        first = false;
        // Append quoted name, escaping backslash and double-quote
        if (pos < static_cast<int>(sizeof(buf)) - 4) buf[pos++] = '"';
        for (int i = 0; name[i] && pos < static_cast<int>(sizeof(buf)) - 4; i++) {
          char c = name[i];
          if (c == '"' || c == '\\') {
            buf[pos++] = '\\';
          }
          buf[pos++] = c;
        }
        if (pos < static_cast<int>(sizeof(buf)) - 4) buf[pos++] = '"';
      }
      entry.close();
    }
    dir.close();
  }

  if (pos < static_cast<int>(sizeof(buf)) - 2) buf[pos++] = ']';
  buf[pos] = '\0';

  server->send(200, "application/json", buf);
}

void DeadDropActivity::handleDownload() {
  if (!server) return;

  if (!server->hasArg("file")) {
    server->send(400, "text/plain", "Missing file param");
    return;
  }

  String fname = server->arg("file");
  // Strip path separators for safety
  int lastSlash = fname.lastIndexOf('/');
  if (lastSlash >= 0) fname = fname.substring(lastSlash + 1);

  String filePath = DROP_DIR + fname;

  if (!Storage.exists(filePath.c_str())) {
    server->send(404, "text/plain", "Not found");
    return;
  }

  HalFile f = Storage.open(filePath.c_str(), O_RDONLY);
  if (!f) {
    server->send(500, "text/plain", "Open failed");
    return;
  }

  String contentDisp = "attachment; filename=\"" + fname + "\"";
  server->sendHeader("Content-Disposition", contentDisp);

  // HalFile doesn't support streamFile() — stream manually
  size_t fileSize = f.size();
  server->setContentLength(fileSize);
  server->send(200, "application/octet-stream", "");

  uint8_t streamBuf[512];
  while (fileSize > 0) {
    size_t chunk = (fileSize < sizeof(streamBuf)) ? fileSize : sizeof(streamBuf);
    int rd = f.read(streamBuf, chunk);
    if (rd <= 0) break;
    server->client().write(streamBuf, rd);
    fileSize -= rd;
  }
  f.close();
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

void DeadDropActivity::loop() {
  if (state == CONFIG) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }

    buttonNavigator.onNext([this] {
      configIndex = ButtonNavigator::nextIndex(configIndex, 2);
      requestUpdate();
    });
    buttonNavigator.onPrevious([this] {
      configIndex = ButtonNavigator::previousIndex(configIndex, 2);
      requestUpdate();
    });

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (configIndex == 0) {
        // Cycle timer option
        timerIndex = ButtonNavigator::nextIndex(timerIndex, TIMER_COUNT);
        timerMinutes = TIMER_OPTIONS[timerIndex];
        requestUpdate();
      } else {
        // Start drop
        startDrop();
        requestUpdate();
      }
    }
    return;
  }

  if (state == ACTIVE) {
    if (server) server->handleClient();
    connectedClients = WiFi.softAPgetStationNum();

    // Check timer expiry
    unsigned long elapsed = millis() - startTime;
    if (elapsed >= static_cast<unsigned long>(timerMinutes) * 60000UL) {
      stopDrop();
      state = DONE;
      requestUpdate();
      return;
    }

    // Back button — stop early
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      stopDrop();
      state = DONE;
      requestUpdate();
      return;
    }

    // Periodic display refresh every 2 seconds
    static unsigned long lastUpdate = 0;
    if (millis() - lastUpdate >= 2000) {
      lastUpdate = millis();
      requestUpdate();
    }
    return;
  }

  if (state == DONE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      finish();
    }
    return;
  }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void DeadDropActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Dead Drop");

  switch (state) {
    case CONFIG:
      renderConfig();
      break;
    case ACTIVE:
      renderActive();
      break;
    case DONE:
      renderDone();
      break;
  }

  renderer.displayBuffer();
}

void DeadDropActivity::renderConfig() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  // Build timer label inline
  char timerLabel[32];
  snprintf(timerLabel, sizeof(timerLabel), "Timer: %d min", timerMinutes);

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, 2, configIndex,
      [&](int i) -> std::string {
        if (i == 0) return timerLabel;
        return "Start Dead Drop";
      });

  const auto labels = mappedInput.mapLabels("Back", "Select", "^", "v");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void DeadDropActivity::renderActive() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Compute time remaining
  unsigned long elapsed = millis() - startTime;
  unsigned long totalMs = static_cast<unsigned long>(timerMinutes) * 60000UL;
  unsigned long remainMs = (elapsed < totalMs) ? (totalMs - elapsed) : 0;
  unsigned int remainSec = static_cast<unsigned int>(remainMs / 1000);
  unsigned int remMin = remainSec / 60;
  unsigned int remSec = remainSec % 60;

  char timeBuf[16];
  snprintf(timeBuf, sizeof(timeBuf), "%02u:%02u", remMin, remSec);

  // Layout: centered block starting just below header
  const int startY = metrics.topPadding + metrics.headerHeight + 20;
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int lineH12 = renderer.getLineHeight(UI_12_FONT_ID);
  int y = startY;

  // SSID — large and bold
  renderer.drawCenteredText(UI_12_FONT_ID, y, ssidName.c_str(), true, EpdFontFamily::BOLD);
  y += lineH12 + 8;

  // IP address
  renderer.drawCenteredText(UI_10_FONT_ID, y, "192.168.4.1", true);
  y += lineH + 16;

  // Time remaining — prominent
  renderer.drawCenteredText(UI_12_FONT_ID, y, "Time left:", true);
  y += lineH12 + 4;
  renderer.drawCenteredText(UI_12_FONT_ID, y, timeBuf, true, EpdFontFamily::BOLD);
  y += lineH12 + 16;

  // Files received
  char filesBuf[32];
  snprintf(filesBuf, sizeof(filesBuf), "Files: %d", filesReceived);
  renderer.drawCenteredText(UI_10_FONT_ID, y, filesBuf, true);
  y += lineH + 8;

  // Connected clients
  char clientsBuf[32];
  snprintf(clientsBuf, sizeof(clientsBuf), "Clients: %d", connectedClients);
  renderer.drawCenteredText(UI_10_FONT_ID, y, clientsBuf, true);

  const auto labels = mappedInput.mapLabels("Stop", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void DeadDropActivity::renderDone() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  const int lineH12 = renderer.getLineHeight(UI_12_FONT_ID);
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);

  int y = pageHeight / 2 - 60;

  renderer.drawCenteredText(UI_12_FONT_ID, y, "Drop Complete", true, EpdFontFamily::BOLD);
  y += lineH12 + 16;

  char filesBuf[32];
  snprintf(filesBuf, sizeof(filesBuf), "Files received: %d", filesReceived);
  renderer.drawCenteredText(UI_10_FONT_ID, y, filesBuf, true);
  y += lineH + 8;

  renderer.drawCenteredText(UI_10_FONT_ID, y, ssidName.c_str(), true);

  const auto labels = mappedInput.mapLabels("Back", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
