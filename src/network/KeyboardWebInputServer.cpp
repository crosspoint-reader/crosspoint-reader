#include "KeyboardWebInputServer.h"

#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_random.h>
#include <esp_wifi.h>

#include "NetworkConstants.h"
#include "html/TextInputPageHtml.generated.h"

KeyboardWebInputServer::~KeyboardWebInputServer() { stop(); }

bool KeyboardWebInputServer::start() {
  if (running) {
    return true;
  }

  Serial.printf("[%lu] [KB-WEB] Starting keyboard web input server...\n", millis());

  // Save current WiFi sleep mode to restore later
  wifi_ps_type_t psType;
  if (esp_wifi_get_ps(&psType) == ESP_OK) {
    previousSleepMode = psType;
  }

  // Check if WiFi is already connected in STA mode
  const wifi_mode_t wifiMode = WiFi.getMode();
  const bool isStaConnected = (wifiMode & WIFI_MODE_STA) && (WiFi.status() == WL_CONNECTED);

  if (isStaConnected) {
    // Reuse existing WiFi connection
    apModeStarted = false;
    ipAddress = WiFi.localIP().toString().c_str();
    Serial.printf("[%lu] [KB-WEB] Using existing STA connection, IP: %s\n", millis(), ipAddress.c_str());
  } else {
    // Start our own Access Point
    Serial.printf("[%lu] [KB-WEB] No WiFi connection, starting AP...\n", millis());

    WiFi.mode(WIFI_AP);
    delay(100);

    if (NetworkConstants::AP_PASSWORD && strlen(NetworkConstants::AP_PASSWORD) >= 8) {
      WiFi.softAP(NetworkConstants::AP_SSID, NetworkConstants::AP_PASSWORD, NetworkConstants::AP_CHANNEL, false,
                  NetworkConstants::AP_MAX_CONNECTIONS);
    } else {
      WiFi.softAP(NetworkConstants::AP_SSID, nullptr, NetworkConstants::AP_CHANNEL, false,
                  NetworkConstants::AP_MAX_CONNECTIONS);
    }

    // Wait for AP to fully initialize
    delay(100);

    const IPAddress apIP = WiFi.softAPIP();
    ipAddress = apIP.toString().c_str();
    apModeStarted = true;

    Serial.printf("[%lu] [KB-WEB] AP started - SSID: %s, IP: %s\n", millis(), NetworkConstants::AP_SSID,
                  ipAddress.c_str());

    // Start DNS server for captive portal behavior
    // This redirects all DNS queries to our IP, making any domain resolve to us
    dnsServer.reset(new DNSServer());
    dnsServer->setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer->start(NetworkConstants::DNS_PORT, "*", apIP);
    Serial.printf("[%lu] [KB-WEB] DNS server started for captive portal\n", millis());
  }

  // Start mDNS
  if (MDNS.begin(NetworkConstants::AP_HOSTNAME)) {
    MDNS.addService("http", "tcp", NetworkConstants::HTTP_PORT);
    Serial.printf("[%lu] [KB-WEB] mDNS started: http://%s.local/\n", millis(), NetworkConstants::AP_HOSTNAME);
  }

  // Disable WiFi sleep for responsiveness
  WiFi.setSleep(false);

  // Generate a session PIN for request validation
  sessionPin = generateSessionPin();
  Serial.printf("[%lu] [KB-WEB] Session PIN: %s\n", millis(), sessionPin.c_str());

  // Create and start web server
  server.reset(new WebServer(NetworkConstants::HTTP_PORT));
  setupRoutes();
  server->begin();

  running = true;
  textReceived = false;
  receivedText.clear();

  Serial.printf("[%lu] [KB-WEB] Server started on port %d\n", millis(), NetworkConstants::HTTP_PORT);
  return true;
}

void KeyboardWebInputServer::stop() {
  if (!running) {
    return;
  }

  Serial.printf("[%lu] [KB-WEB] Stopping keyboard web input server...\n", millis());

  if (server) {
    server->stop();
    server.reset();
  }

  MDNS.end();

  // Stop DNS server if running (AP mode captive portal)
  if (dnsServer) {
    dnsServer->stop();
    dnsServer.reset();
    Serial.printf("[%lu] [KB-WEB] DNS server stopped\n", millis());
  }

  // Brief wait for LWIP stack to flush pending packets
  delay(50);

  if (apModeStarted) {
    WiFi.softAPdisconnect(true);
    delay(30);
    WiFi.mode(WIFI_OFF);
    apModeStarted = false;
    Serial.printf("[%lu] [KB-WEB] AP stopped\n", millis());
  } else {
    // Restore previous WiFi sleep mode
    esp_wifi_set_ps(previousSleepMode);
  }

  running = false;
  Serial.printf("[%lu] [KB-WEB] Server stopped\n", millis());
}

void KeyboardWebInputServer::handleClient() {
  if (running && server) {
    server->handleClient();
  }
  // Process DNS requests for captive portal (AP mode)
  if (running && dnsServer) {
    dnsServer->processNextRequest();
  }
}

std::string KeyboardWebInputServer::consumeReceivedText() {
  textReceived = false;
  std::string result = std::move(receivedText);
  receivedText.clear();
  return result;
}

std::string KeyboardWebInputServer::getApSSID() const { return NetworkConstants::AP_SSID; }

std::string KeyboardWebInputServer::getUrl() const {
  if (apModeStarted) {
    return std::string("http://") + NetworkConstants::AP_HOSTNAME + ".local/";
  }
  return "http://" + ipAddress + "/";
}

std::string KeyboardWebInputServer::getWifiQRString() const {
  const std::string escapedSsid = escapeWifiSpecialChars(NetworkConstants::AP_SSID);
  return "WIFI:T:nopass;S:" + escapedSsid + ";;";
}

std::string KeyboardWebInputServer::escapeWifiSpecialChars(const std::string& input) {
  std::string result;
  result.reserve(input.size());
  for (const char c : input) {
    if (c == '\\' || c == ';' || c == ',' || c == ':' || c == '"') {
      result += '\\';
    }
    result += c;
  }
  return result;
}

std::string KeyboardWebInputServer::generateSessionPin() {
  const uint32_t pin = esp_random() % 10000;
  char buf[5];
  snprintf(buf, sizeof(buf), "%04u", pin);
  return std::string(buf);
}

void KeyboardWebInputServer::setupRoutes() {
  server->on("/", HTTP_GET, [this] { handleRootPage(); });
  server->on("/api/keyboard-input", HTTP_POST, [this] { handleTextSubmit(); });

  // Captive portal: redirect any unknown page to root
  server->onNotFound([this] {
    server->sendHeader("Location", "/", true);
    server->send(302, "text/plain", "Redirecting...");
  });
}

void KeyboardWebInputServer::handleRootPage() {
  // Inject session PIN into the page so the client can include it in requests
  String html(TextInputPageHtml);
  html.replace("{{SESSION_PIN}}", sessionPin.c_str());
  server->send(200, "text/html", html);
  Serial.printf("[%lu] [KB-WEB] Served text input page\n", millis());
}

void KeyboardWebInputServer::handleTextSubmit() {
  // Validate session PIN
  if (!server->hasArg("pin") || server->arg("pin").c_str() != sessionPin) {
    Serial.printf("[%lu] [KB-WEB] Rejected request: invalid or missing session PIN\n", millis());
    server->send(403, "text/plain", "Invalid session PIN");
    return;
  }

  if (!server->hasArg("text")) {
    server->send(400, "text/plain", "Missing 'text' parameter");
    return;
  }

  receivedText = server->arg("text").c_str();
  textReceived = true;

  Serial.printf("[%lu] [KB-WEB] Received text (%zu chars)\n", millis(), receivedText.length());

  server->send(200, "text/plain", "OK");
}
