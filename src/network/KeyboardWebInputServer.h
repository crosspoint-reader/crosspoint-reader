#pragma once

#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>

#include <memory>
#include <string>

/**
 * Lightweight web server for remote text input via a phone/browser.
 * Used by KeyboardEntryActivity to allow users to type text on their
 * phone instead of using the on-screen keyboard on the e-ink display.
 *
 * If WiFi is already connected (STA mode), it reuses the existing connection.
 * Otherwise, it creates a WiFi Access Point so the phone can connect directly.
 */
class KeyboardWebInputServer {
 public:
  KeyboardWebInputServer() = default;
  ~KeyboardWebInputServer();

  /**
   * Start the server. Creates an AP if WiFi is not already connected.
   * @return true if started successfully
   */
  bool start();

  /**
   * Stop the server and clean up WiFi AP if we started one.
   */
  void stop();

  /**
   * Call periodically from the activity loop to handle incoming HTTP requests.
   */
  void handleClient();

  /**
   * Check if text has been received since last call to consumeReceivedText().
   */
  bool hasReceivedText() const { return textReceived; }

  /**
   * Get the received text and clear the received flag.
   */
  std::string consumeReceivedText();

  /**
   * Get the URL for QR code display.
   */
  std::string getUrl() const;

  /**
   * Check if the server started its own AP (vs reusing STA connection).
   */
  bool isApMode() const { return apModeStarted; }

  /**
   * Get the AP SSID (for WiFi QR code). Only meaningful if isApMode() is true.
   */
  std::string getApSSID() const;

  /**
   * Get the WiFi QR code string for connecting to the AP.
   * Format: WIFI:T:nopass;S:<ssid>;;
   */
  std::string getWifiQRString() const;

  /**
   * Get the session PIN displayed on screen for request validation.
   */
  std::string getSessionPin() const { return sessionPin; }

  /**
   * Get the device IP address.
   */
  std::string getIP() const { return ipAddress; }

  bool isRunning() const { return running; }

 private:
  std::unique_ptr<WebServer> server;
  std::unique_ptr<DNSServer> dnsServer;
  bool running = false;
  bool apModeStarted = false;
  bool textReceived = false;
  std::string receivedText;
  std::string ipAddress;
  std::string sessionPin;
  wifi_ps_type_t previousSleepMode = WIFI_PS_NONE;

  static std::string generateSessionPin();
  static std::string escapeWifiSpecialChars(const std::string& input);
  void setupRoutes();
  void handleRootPage();
  void handleTextSubmit();
};
