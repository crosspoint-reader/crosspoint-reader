#include "BulletinBoardActivity.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_random.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/RadioManager.h"

// HTML page served to clients — stored in flash via PROGMEM
static const char BOARD_HTML[] PROGMEM =
    "<!DOCTYPE html><html><head><meta name=\"viewport\" content=\"width=device-width\">"
    "<title>Bulletin Board</title>"
    "<style>"
    "body{font-family:sans-serif;max-width:600px;margin:0 auto;padding:16px;background:#f5f5f5}"
    "h1{font-size:1.4em;margin-bottom:8px}"
    ".post{background:white;padding:8px 12px;margin:6px 0;border-radius:4px;border:1px solid #ddd}"
    ".time{color:#999;font-size:0.8em}"
    "form{display:flex;gap:8px;margin:12px 0}"
    "input[type=text]{flex:1;padding:8px;border:1px solid #ccc;border-radius:4px}"
    "button{padding:8px 16px;background:#333;color:white;border:none;border-radius:4px}"
    "</style></head>"
    "<body><h1>Bulletin Board</h1>"
    "<form method=\"POST\" action=\"/post\">"
    "<input type=\"text\" name=\"msg\" maxlength=\"200\" placeholder=\"Write a message...\" autofocus>"
    "<button type=\"submit\">Post</button></form>"
    "<div id=\"board\">Loading...</div>"
    "<script>"
    "function load(){fetch('/posts').then(r=>r.json()).then(posts=>{"
    "let h='';posts.forEach(p=>{"
    "let d=new Date(p.t*1000);let ts=d.getHours()+':'+String(d.getMinutes()).padStart(2,'0');"
    "h+='<div class=\"post\">'+p.m+'<div class=\"time\">'+ts+'</div></div>'});"
    "document.getElementById('board').innerHTML=h||'<p>No messages yet. Be the first!</p>'})}"
    "load();setInterval(load,3000);"
    "</script></body></html>";

// Static instance pointer for web handler lambdas
BulletinBoardActivity* BulletinBoardActivity::activeInstance = nullptr;

// ODR definition for constexpr array
constexpr int BulletinBoardActivity::TIMER_OPTIONS[];

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void BulletinBoardActivity::onEnter() {
  Activity::onEnter();
  state = CONFIG;
  configIndex = 0;
  timerIndex = 1;
  timerMinutes = TIMER_OPTIONS[timerIndex];
  finalPostCount = 0;
  posts.clear();
  ssidName.clear();
  requestUpdate();
}

void BulletinBoardActivity::onExit() {
  Activity::onExit();
  if (state == ACTIVE) {
    stopBoard();
  }
}

// ---------------------------------------------------------------------------
// startBoard / stopBoard
// ---------------------------------------------------------------------------

void BulletinBoardActivity::startBoard() {
  posts.clear();

  // Generate random SSID suffix
  char suffix[5];
  snprintf(suffix, sizeof(suffix), "%04X", static_cast<uint16_t>(esp_random() & 0xFFFF));
  ssidName = std::string("BOARD-") + suffix;

  RADIO.ensureWifi();
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssidName.c_str());

  setupWebServer();

  startTime = millis();
  state = ACTIVE;
  activeInstance = this;
}

void BulletinBoardActivity::stopBoard() {
  finalPostCount = static_cast<int>(posts.size());
  if (server) {
    server->stop();
    delete server;
    server = nullptr;
  }
  WiFi.softAPdisconnect(true);
  RADIO.shutdown();
  posts.clear();
  activeInstance = nullptr;
}

// ---------------------------------------------------------------------------
// Web server setup
// ---------------------------------------------------------------------------

void BulletinBoardActivity::setupWebServer() {
  server = new WebServer(80);

  // GET / — serve bulletin board HTML
  server->on("/", HTTP_GET, [this] {
    if (server) server->send_P(200, "text/html", BOARD_HTML);
  });

  // POST /post — receive a message from the form
  server->on("/post", HTTP_POST, [this] {
    if (!server) return;
    if (server->hasArg("msg")) {
      String msg = server->arg("msg");
      msg.trim();
      if (msg.length() > 0 && static_cast<int>(posts.size()) < MAX_POSTS) {
        Post p = {};
        strncpy(p.text, msg.c_str(), 200);
        p.text[200] = '\0';
        p.timestamp = millis() / 1000;
        posts.push_back(p);
        LOG_DBG("BOARD", "Post added, total=%d", (int)posts.size());
      }
    }
    server->sendHeader("Location", "/");
    server->send(303);
  });

  // GET /posts — return JSON array for AJAX polling
  server->on("/posts", HTTP_GET, [this] {
    if (!server) return;

    // Build JSON manually to avoid ArduinoJson heap usage
    std::string json = "[";
    for (size_t i = 0; i < posts.size(); i++) {
      if (i > 0) json += ",";
      json += "{\"m\":\"";
      for (const char* p = posts[i].text; *p; p++) {
        if (*p == '"')       json += "\\\"";
        else if (*p == '\\') json += "\\\\";
        else if (*p == '\n') json += "\\n";
        else if (*p == '<')  json += "&lt;";
        else if (*p == '>')  json += "&gt;";
        else                 json += *p;
      }
      json += "\",\"t\":";
      char tsBuf[16];
      snprintf(tsBuf, sizeof(tsBuf), "%lu", posts[i].timestamp);
      json += tsBuf;
      json += "}";
    }
    json += "]";
    server->send(200, "application/json", json.c_str());
  });

  server->begin();
  WiFi.setSleep(false);
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

void BulletinBoardActivity::loop() {
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
        // Start board
        startBoard();
        requestUpdate();
      }
    }
    return;
  }

  if (state == ACTIVE) {
    if (server) server->handleClient();

    // Check timer expiry
    unsigned long elapsed = millis() - startTime;
    if (elapsed >= static_cast<unsigned long>(timerMinutes) * 60000UL) {
      stopBoard();
      state = DONE;
      requestUpdate();
      return;
    }

    // Back button — stop early
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      stopBoard();
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

void BulletinBoardActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Bulletin Board");

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

void BulletinBoardActivity::renderConfig() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  char timerLabel[32];
  snprintf(timerLabel, sizeof(timerLabel), "Timer: %d min", timerMinutes);

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, 2, configIndex,
      [&](int i) -> std::string {
        if (i == 0) return timerLabel;
        return "Start Bulletin Board";
      });

  const auto labels = mappedInput.mapLabels("Back", "Select", "^", "v");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void BulletinBoardActivity::renderActive() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  // Compute time remaining
  unsigned long elapsed = millis() - startTime;
  unsigned long totalMs = static_cast<unsigned long>(timerMinutes) * 60000UL;
  unsigned long remainMs = (elapsed < totalMs) ? (totalMs - elapsed) : 0;
  unsigned int remainSec = static_cast<unsigned int>(remainMs / 1000);
  unsigned int remMin = remainSec / 60;
  unsigned int remSec = remainSec % 60;

  char timeBuf[16];
  snprintf(timeBuf, sizeof(timeBuf), "%02u:%02u", remMin, remSec);

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

  // Time remaining
  renderer.drawCenteredText(UI_12_FONT_ID, y, "Time left:", true);
  y += lineH12 + 4;
  renderer.drawCenteredText(UI_12_FONT_ID, y, timeBuf, true, EpdFontFamily::BOLD);
  y += lineH12 + 16;

  // Post count
  char postsBuf[32];
  snprintf(postsBuf, sizeof(postsBuf), "Posts: %d", (int)posts.size());
  renderer.drawCenteredText(UI_10_FONT_ID, y, postsBuf, true);
  y += lineH + 8;

  // Connected clients
  char clientsBuf[32];
  snprintf(clientsBuf, sizeof(clientsBuf), "Clients: %d", (int)WiFi.softAPgetStationNum());
  renderer.drawCenteredText(UI_10_FONT_ID, y, clientsBuf, true);

  const auto labels = mappedInput.mapLabels("Stop", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void BulletinBoardActivity::renderDone() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageHeight = renderer.getScreenHeight();

  const int lineH12 = renderer.getLineHeight(UI_12_FONT_ID);
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);

  int y = pageHeight / 2 - 60;

  renderer.drawCenteredText(UI_12_FONT_ID, y, "Board Closed", true, EpdFontFamily::BOLD);
  y += lineH12 + 16;

  char postsBuf[32];
  snprintf(postsBuf, sizeof(postsBuf), "Posts received: %d", finalPostCount);
  renderer.drawCenteredText(UI_10_FONT_ID, y, postsBuf, true);
  y += lineH + 8;

  renderer.drawCenteredText(UI_10_FONT_ID, y, ssidName.c_str(), true);

  const auto labels = mappedInput.mapLabels("Back", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
