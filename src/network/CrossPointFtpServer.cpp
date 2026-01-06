#include "CrossPointFtpServer.h"

#include <HardwareSerial.h>
#include <SDCardManager.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include "../CrossPointSettings.h"

CrossPointFtpServer::~CrossPointFtpServer() { stop(); }

bool CrossPointFtpServer::begin() {
  if (isRunning) {
    Serial.printf("[%lu] [FTP] Server already running\n", millis());
    return true;
  }

  // Check WiFi connection
  if (WiFi.status() != WL_CONNECTED && WiFi.getMode() != WIFI_AP) {
    Serial.printf("[%lu] [FTP] WiFi not connected\n", millis());
    return false;
  }

  // Disable WiFi sleep for better responsiveness
  esp_wifi_set_ps(WIFI_PS_NONE);

  Serial.printf("[%lu] [FTP] Free heap before starting: %lu bytes\n", millis(), ESP.getFreeHeap());

  // Create and start FTP server
  ftpServer = new FtpServer();

  // Start FTP server with credentials from settings
  // The library automatically uses the global SdFat2 filesystem
  ftpServer->begin(SETTINGS.ftpUsername.c_str(), SETTINGS.ftpPassword.c_str());

  isRunning = true;

  Serial.printf("[%lu] [FTP] Server started on port 21\n", millis());
  Serial.printf("[%lu] [FTP] Username: %s\n", millis(), SETTINGS.ftpUsername.c_str());
  Serial.printf("[%lu] [FTP] Free heap after starting: %lu bytes\n", millis(), ESP.getFreeHeap());

  return true;
}

void CrossPointFtpServer::stop() {
  if (!isRunning) {
    return;
  }

  if (ftpServer) {
    delete ftpServer;
    ftpServer = nullptr;
  }

  isRunning = false;
  Serial.printf("[%lu] [FTP] Server stopped\n", millis());
}

void CrossPointFtpServer::handleClient() {
  if (isRunning && ftpServer) {
    ftpServer->handleFTP();
  }
}
