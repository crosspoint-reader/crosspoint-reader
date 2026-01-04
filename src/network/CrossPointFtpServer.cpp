#include "CrossPointFtpServer.h"

#include <WiFi.h>

namespace {
// FTP server credentials
constexpr const char* FTP_USERNAME = "crosspoint";
constexpr const char* FTP_PASSWORD = "reader";
}  // namespace

CrossPointFtpServer::CrossPointFtpServer() {}

CrossPointFtpServer::~CrossPointFtpServer() { stop(); }

void CrossPointFtpServer::begin() {
  if (running) {
    Serial.printf("[%lu] [FTP] FTP server already running\n", millis());
    return;
  }

  // Check if we have a valid network connection (either STA connected or AP mode)
  const wifi_mode_t wifiMode = WiFi.getMode();
  const bool isStaConnected = (wifiMode & WIFI_MODE_STA) && (WiFi.status() == WL_CONNECTED);
  const bool isInApMode = (wifiMode & WIFI_MODE_AP) && (WiFi.softAPgetStationNum() >= 0);  // AP is running

  if (!isStaConnected && !isInApMode) {
    Serial.printf("[%lu] [FTP] Cannot start FTP server - no valid network (mode=%d, status=%d)\n", millis(), wifiMode,
                  WiFi.status());
    return;
  }

  // Store AP mode flag for later use
  apMode = isInApMode;

  Serial.printf("[%lu] [FTP] [MEM] Free heap before begin: %d bytes\n", millis(), ESP.getFreeHeap());
  Serial.printf("[%lu] [FTP] Network mode: %s\n", millis(), apMode ? "AP" : "STA");

  Serial.printf("[%lu] [FTP] Creating FTP server on port 21...\n", millis());

  // Create FTP server instance
  ftpServer.reset(new ::FtpServer());

  // Disable WiFi sleep to improve responsiveness and prevent 'unreachable' errors.
  // This is critical for reliable FTP server operation on ESP32.
  WiFi.setSleep(false);

  Serial.printf("[%lu] [FTP] [MEM] Free heap after FTPServer allocation: %d bytes\n", millis(), ESP.getFreeHeap());

  if (!ftpServer) {
    Serial.printf("[%lu] [FTP] Failed to create FTPServer!\n", millis());
    return;
  }

  // Initialize FTP server with credentials
  ftpServer->begin(FTP_USERNAME, FTP_PASSWORD);
  running = true;

  Serial.printf("[%lu] [FTP] FTP server started on port 21\n", millis());
  // Show the correct IP based on network mode
  const String ipAddr = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  Serial.printf("[%lu] [FTP] Access at ftp://%s/\n", millis(), ipAddr.c_str());
  Serial.printf("[%lu] [FTP] Username: %s\n", millis(), FTP_USERNAME);
  Serial.printf("[%lu] [FTP] Password: %s\n", millis(), FTP_PASSWORD);
  Serial.printf("[%lu] [FTP] [MEM] Free heap after server.begin(): %d bytes\n", millis(), ESP.getFreeHeap());
}

void CrossPointFtpServer::stop() {
  if (!running || !ftpServer) {
    Serial.printf("[%lu] [FTP] stop() called but already stopped (running=%d, ftpServer=%p)\n", millis(), running,
                  ftpServer.get());
    return;
  }

  Serial.printf("[%lu] [FTP] STOP INITIATED - setting running=false first\n", millis());
  running = false;  // Set this FIRST to prevent handleClient from using server

  Serial.printf("[%lu] [FTP] [MEM] Free heap before stop: %d bytes\n", millis(), ESP.getFreeHeap());

  // Add delay to allow any in-flight handleClient() calls to complete
  delay(100);
  Serial.printf("[%lu] [FTP] Waited 100ms for handleClient to finish\n", millis());

  // SimpleFTPServer doesn't have explicit stop method, just delete
  ftpServer.reset();
  Serial.printf("[%lu] [FTP] FTP server stopped and deleted\n", millis());
  Serial.printf("[%lu] [FTP] [MEM] Free heap after delete server: %d bytes\n", millis(), ESP.getFreeHeap());
  Serial.printf("[%lu] [FTP] [MEM] Free heap final: %d bytes\n", millis(), ESP.getFreeHeap());
}

void CrossPointFtpServer::handleClient() const {
  static unsigned long lastDebugPrint = 0;

  // Check running flag FIRST before accessing server
  if (!running) {
    return;
  }

  // Double-check server pointer is valid
  if (!ftpServer) {
    Serial.printf("[%lu] [FTP] WARNING: handleClient called with null server!\n", millis());
    return;
  }

  // Print debug every 10 seconds to confirm handleClient is being called
  if (millis() - lastDebugPrint > 10000) {
    Serial.printf("[%lu] [FTP] handleClient active, server running on port 21\n", millis());
    lastDebugPrint = millis();
  }

  ftpServer->handleFTP();
}
