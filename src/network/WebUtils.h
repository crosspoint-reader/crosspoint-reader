#pragma once

#include <WebServer.h>

#include <cstddef>

// Inline helpers shared between CrossPointWebServer and feature Registration.cpp mountRoutes.

inline bool isGzipPayload(const char* data, const size_t len) {
  return len >= 2 && static_cast<unsigned char>(data[0]) == 0x1f && static_cast<unsigned char>(data[1]) == 0x8b;
}

inline void sendPrecompressedHtml(WebServer* server, const char* data, const size_t compressedLen) {
  if (!isGzipPayload(data, compressedLen)) {
    server->send(500, "text/plain", "Invalid precompressed HTML payload");
    return;
  }
  server->sendHeader("Content-Encoding", "gzip");
  server->send_P(200, "text/html; charset=utf-8", data, compressedLen);
}
