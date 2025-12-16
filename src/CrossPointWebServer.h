#pragma once

#include <WebServer.h>
#include <functional>
#include <string>

class CrossPointWebServer {
 public:
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

  // Get the port number
  uint16_t getPort() const { return port; }

 private:
  WebServer* server = nullptr;
  bool running = false;
  uint16_t port = 80;

  // Request handlers
  void handleRoot();
  void handleNotFound();
  void handleStatus();
};

// Global instance
extern CrossPointWebServer crossPointWebServer;
