#pragma once

#include <functional>
#include <memory>

#include "WString.h"

// Dummy WebSockets object types
enum WStype_t {
  WStype_DISCONNECTED,
  WStype_CONNECTED,
  WStype_TEXT,
  WStype_BIN,
  WStype_ERROR,
  WStype_FRAGMENT_TEXT_START,
  WStype_FRAGMENT_BIN_START,
  WStype_FRAGMENT,
  WStype_FRAGMENT_FIN,
  WStype_PING,
  WStype_PONG,
};

class WebSocketsServer {
public:
  explicit WebSocketsServer(int port);
  ~WebSocketsServer();
  void begin();
  void loop();
  template <typename T> void onEvent(T callback) { callback_ = callback; }
  void broadcastTXT(const String &txt);
  void broadcastTXT(const char *txt);
  void sendTXT(uint8_t num, const String &txt);
  void sendTXT(uint8_t num, const char *txt);
  void close();

private:
  using EventCallback =
      std::function<void(uint8_t, WStype_t, uint8_t *, size_t)>;

  struct Impl;
  std::unique_ptr<Impl> impl_;
  EventCallback callback_;
};
