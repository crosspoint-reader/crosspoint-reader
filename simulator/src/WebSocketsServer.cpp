#include "WebSocketsServer.h"

#include <Logging.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr size_t MAX_WS_PAYLOAD = 256UL * 1024UL * 1024UL;

uint32_t rol32(uint32_t value, uint8_t shift) {
  return (value << shift) | (value >> (32 - shift));
}

std::string lower(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::string trim(std::string value) {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front())))
    value.erase(value.begin());
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back())))
    value.pop_back();
  return value;
}

bool recvAll(int fd, void *data, size_t len) {
  auto *ptr = static_cast<uint8_t *>(data);
  while (len > 0) {
    const ssize_t got = ::recv(fd, ptr, len, 0);
    if (got <= 0)
      return false;
    ptr += got;
    len -= static_cast<size_t>(got);
  }
  return true;
}

bool sendAll(int fd, const void *data, size_t len) {
  const auto *ptr = static_cast<const uint8_t *>(data);
#ifdef MSG_NOSIGNAL
  constexpr int sendFlags = MSG_NOSIGNAL;
#else
  constexpr int sendFlags = 0;
#endif
  while (len > 0) {
    const ssize_t sent = ::send(fd, ptr, len, sendFlags);
    if (sent <= 0)
      return false;
    ptr += sent;
    len -= static_cast<size_t>(sent);
  }
  return true;
}

void setSocketTimeouts(int fd) {
  timeval timeout{};
  timeout.tv_sec = 5;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#ifdef SO_NOSIGPIPE
  int yes = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
}

std::array<uint8_t, 20> sha1Digest(const std::string &input) {
  std::vector<uint8_t> message(input.begin(), input.end());
  const uint64_t bitLength = static_cast<uint64_t>(message.size()) * 8ULL;
  message.push_back(0x80);
  while ((message.size() % 64) != 56)
    message.push_back(0);
  for (int i = 7; i >= 0; --i)
    message.push_back(static_cast<uint8_t>((bitLength >> (i * 8)) & 0xff));

  uint32_t h0 = 0x67452301;
  uint32_t h1 = 0xefcdab89;
  uint32_t h2 = 0x98badcfe;
  uint32_t h3 = 0x10325476;
  uint32_t h4 = 0xc3d2e1f0;

  for (size_t offset = 0; offset < message.size(); offset += 64) {
    uint32_t w[80] = {};
    for (int i = 0; i < 16; ++i) {
      const size_t idx = offset + static_cast<size_t>(i) * 4;
      w[i] = (static_cast<uint32_t>(message[idx]) << 24) |
             (static_cast<uint32_t>(message[idx + 1]) << 16) |
             (static_cast<uint32_t>(message[idx + 2]) << 8) |
             static_cast<uint32_t>(message[idx + 3]);
    }
    for (int i = 16; i < 80; ++i)
      w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = h0;
    uint32_t b = h1;
    uint32_t c = h2;
    uint32_t d = h3;
    uint32_t e = h4;
    for (int i = 0; i < 80; ++i) {
      uint32_t f = 0;
      uint32_t k = 0;
      if (i < 20) {
        f = (b & c) | ((~b) & d);
        k = 0x5a827999;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ed9eba1;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8f1bbcdc;
      } else {
        f = b ^ c ^ d;
        k = 0xca62c1d6;
      }
      const uint32_t temp = rol32(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = rol32(b, 30);
      b = a;
      a = temp;
    }
    h0 += a;
    h1 += b;
    h2 += c;
    h3 += d;
    h4 += e;
  }

  std::array<uint8_t, 20> digest{};
  const uint32_t words[] = {h0, h1, h2, h3, h4};
  for (size_t i = 0; i < 5; ++i) {
    digest[i * 4] = static_cast<uint8_t>((words[i] >> 24) & 0xff);
    digest[i * 4 + 1] = static_cast<uint8_t>((words[i] >> 16) & 0xff);
    digest[i * 4 + 2] = static_cast<uint8_t>((words[i] >> 8) & 0xff);
    digest[i * 4 + 3] = static_cast<uint8_t>(words[i] & 0xff);
  }
  return digest;
}

std::string base64Encode(const uint8_t *data, size_t size) {
  static constexpr char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((size + 2) / 3) * 4);
  for (size_t i = 0; i < size; i += 3) {
    const uint32_t b0 = data[i];
    const uint32_t b1 = (i + 1 < size) ? data[i + 1] : 0;
    const uint32_t b2 = (i + 2 < size) ? data[i + 2] : 0;
    const uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
    out.push_back(alphabet[(triple >> 18) & 0x3f]);
    out.push_back(alphabet[(triple >> 12) & 0x3f]);
    out.push_back(i + 1 < size ? alphabet[(triple >> 6) & 0x3f] : '=');
    out.push_back(i + 2 < size ? alphabet[triple & 0x3f] : '=');
  }
  return out;
}

std::string websocketAcceptValue(const std::string &key) {
  static constexpr char GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  const auto digest = sha1Digest(key + GUID);
  return base64Encode(digest.data(), digest.size());
}

bool sendWsTextFrame(int fd, const std::string &message) {
  std::vector<uint8_t> frame;
  frame.reserve(message.size() + 10);
  frame.push_back(0x81);
  if (message.size() < 126) {
    frame.push_back(static_cast<uint8_t>(message.size()));
  } else if (message.size() <= 0xffff) {
    frame.push_back(126);
    frame.push_back(static_cast<uint8_t>((message.size() >> 8) & 0xff));
    frame.push_back(static_cast<uint8_t>(message.size() & 0xff));
  } else {
    frame.push_back(127);
    for (int i = 7; i >= 0; --i)
      frame.push_back(static_cast<uint8_t>((message.size() >> (i * 8)) & 0xff));
  }
  frame.insert(frame.end(), message.begin(), message.end());
  return sendAll(fd, frame.data(), frame.size());
}

bool sendWsPongFrame(int fd, const std::vector<uint8_t> &payload) {
  std::vector<uint8_t> frame;
  frame.reserve(payload.size() + 10);
  frame.push_back(0x8a);
  if (payload.size() < 126) {
    frame.push_back(static_cast<uint8_t>(payload.size()));
  } else {
    return false;
  }
  frame.insert(frame.end(), payload.begin(), payload.end());
  return sendAll(fd, frame.data(), frame.size());
}

struct WsFrame {
  uint8_t opcode = 0;
  std::vector<uint8_t> payload;
};

bool readWsFrame(int fd, WsFrame &frame) {
  uint8_t header[2] = {};
  if (!recvAll(fd, header, sizeof(header)))
    return false;
  frame.opcode = header[0] & 0x0f;
  const bool masked = (header[1] & 0x80) != 0;
  uint64_t len = header[1] & 0x7f;
  if (len == 126) {
    uint8_t ext[2] = {};
    if (!recvAll(fd, ext, sizeof(ext)))
      return false;
    len = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
  } else if (len == 127) {
    uint8_t ext[8] = {};
    if (!recvAll(fd, ext, sizeof(ext)))
      return false;
    len = 0;
    for (uint8_t b : ext)
      len = (len << 8) | b;
  }
  if (len > MAX_WS_PAYLOAD)
    return false;

  uint8_t mask[4] = {};
  if (masked && !recvAll(fd, mask, sizeof(mask)))
    return false;

  frame.payload.assign(static_cast<size_t>(len), 0);
  if (len > 0 && !recvAll(fd, frame.payload.data(), static_cast<size_t>(len)))
    return false;
  if (masked) {
    for (size_t i = 0; i < frame.payload.size(); ++i)
      frame.payload[i] ^= mask[i % 4];
  }
  return true;
}
} // namespace

struct WebSocketsServer::Impl {
  explicit Impl(int serverPort) : port(serverPort == 81 ? 8081 : serverPort) {}

  struct Event {
    uint8_t num = 0;
    WStype_t type = WStype_ERROR;
    std::vector<uint8_t> payload;
    size_t length = 0;
  };

  int port;
  int fd = -1;
  std::atomic<bool> active{false};
  std::thread acceptThread;
  std::mutex clientsMutex;
  std::mutex clientThreadsMutex;
  std::mutex eventsMutex;
  std::map<uint8_t, int> clients;
  std::vector<std::thread> clientThreads;
  std::vector<Event> pendingEvents;
  uint8_t nextClient = 0;

  uint8_t addClient(int clientFd) {
    std::lock_guard<std::mutex> lock(clientsMutex);
    for (size_t i = 0; i < 255; ++i) {
      const uint8_t candidate = nextClient++;
      if (clients.find(candidate) == clients.end()) {
        clients[candidate] = clientFd;
        return candidate;
      }
    }
    clients[nextClient] = clientFd;
    return nextClient++;
  }

  void removeClient(uint8_t num) {
    std::lock_guard<std::mutex> lock(clientsMutex);
    clients.erase(num);
  }

  std::vector<int> clientFds() {
    std::lock_guard<std::mutex> lock(clientsMutex);
    std::vector<int> out;
    out.reserve(clients.size());
    for (const auto &client : clients)
      out.push_back(client.second);
    return out;
  }

  int clientFd(uint8_t num) {
    std::lock_guard<std::mutex> lock(clientsMutex);
    const auto it = clients.find(num);
    return it == clients.end() ? -1 : it->second;
  }

  void pushEvent(Event event) {
    std::lock_guard<std::mutex> lock(eventsMutex);
    pendingEvents.push_back(std::move(event));
  }

  std::vector<Event> drainEvents() {
    std::lock_guard<std::mutex> lock(eventsMutex);
    std::vector<Event> out;
    out.swap(pendingEvents);
    return out;
  }
};

WebSocketsServer::WebSocketsServer(int port)
    : impl_(std::make_unique<Impl>(port)) {}

WebSocketsServer::~WebSocketsServer() { close(); }

void WebSocketsServer::begin() {
  if (impl_->active)
    return;

  impl_->fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (impl_->fd < 0) {
    LOG_ERR("WS", "[SIM] WebSocket socket failed: %s", strerror(errno));
    return;
  }

  int yes = 1;
  setsockopt(impl_->fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(impl_->port);
  if (::bind(impl_->fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) !=
      0) {
    LOG_ERR("WS", "[SIM] WebSocket bind 127.0.0.1:%d failed: %s", impl_->port,
            strerror(errno));
    ::close(impl_->fd);
    impl_->fd = -1;
    return;
  }
  if (::listen(impl_->fd, 8) != 0) {
    LOG_ERR("WS", "[SIM] WebSocket listen failed: %s", strerror(errno));
    ::close(impl_->fd);
    impl_->fd = -1;
    return;
  }

  impl_->active = true;
  impl_->acceptThread = std::thread([this] {
    while (impl_->active) {
      const int client = ::accept(impl_->fd, nullptr, nullptr);
      if (client < 0) {
        if (impl_->active)
          LOG_ERR("WS", "[SIM] WebSocket accept failed: %s", strerror(errno));
        continue;
      }
      setSocketTimeouts(client);

      std::lock_guard<std::mutex> lock(impl_->clientThreadsMutex);
      impl_->clientThreads.emplace_back([this, client] {
        std::string raw;
        char buffer[2048];
        while (raw.find("\r\n\r\n") == std::string::npos) {
          const ssize_t got = ::recv(client, buffer, sizeof(buffer), 0);
          if (got <= 0) {
            ::close(client);
            return;
          }
          raw.append(buffer, static_cast<size_t>(got));
          if (raw.size() > 32768) {
            ::close(client);
            return;
          }
        }

        const size_t headerEnd = raw.find("\r\n\r\n");
        std::istringstream headerStream(raw.substr(0, headerEnd));
        std::string line;
        std::getline(headerStream, line);

        std::map<std::string, std::string> headers;
        while (std::getline(headerStream, line)) {
          if (!line.empty() && line.back() == '\r')
            line.pop_back();
          const size_t colon = line.find(':');
          if (colon == std::string::npos)
            continue;
          headers[lower(line.substr(0, colon))] = trim(line.substr(colon + 1));
        }

        const auto keyIt = headers.find("sec-websocket-key");
        if (keyIt == headers.end()) {
          ::close(client);
          return;
        }

        std::ostringstream response;
        response << "HTTP/1.1 101 Switching Protocols\r\n"
                 << "Upgrade: websocket\r\n"
                 << "Connection: Upgrade\r\n"
                 << "Sec-WebSocket-Accept: "
                 << websocketAcceptValue(keyIt->second) << "\r\n\r\n";
        if (!sendAll(client, response.str().data(), response.str().size())) {
          ::close(client);
          return;
        }

        const uint8_t num = impl_->addClient(client);
        impl_->pushEvent({num, WStype_CONNECTED, {}, 0});

        WsFrame frame;
        while (impl_->active && readWsFrame(client, frame)) {
          if (frame.opcode == 0x1) {
            const size_t length = frame.payload.size();
            frame.payload.push_back(0);
            impl_->pushEvent(
                {num, WStype_TEXT, std::move(frame.payload), length});
          } else if (frame.opcode == 0x2) {
            const size_t length = frame.payload.size();
            impl_->pushEvent(
                {num, WStype_BIN, std::move(frame.payload), length});
          } else if (frame.opcode == 0x8) {
            break;
          } else if (frame.opcode == 0x9) {
            sendWsPongFrame(client, frame.payload);
          }
        }

        impl_->pushEvent({num, WStype_DISCONNECTED, {}, 0});
        impl_->removeClient(num);
        ::close(client);
      });
    }
  });

  LOG_DBG("WS", "[SIM] WebSocket server running at ws://127.0.0.1:%d/",
          impl_->port);
}

void WebSocketsServer::loop() {
  if (!callback_)
    return;
  auto events = impl_->drainEvents();
  for (auto &event : events) {
    uint8_t *payload = event.payload.empty() ? nullptr : event.payload.data();
    callback_(event.num, event.type, payload, event.length);
  }
}

void WebSocketsServer::broadcastTXT(const String &txt) {
  broadcastTXT(txt.c_str());
}

void WebSocketsServer::broadcastTXT(const char *txt) {
  const std::string message = txt ? txt : "";
  for (const int fd : impl_->clientFds())
    sendWsTextFrame(fd, message);
}

void WebSocketsServer::sendTXT(uint8_t num, const String &txt) {
  sendTXT(num, txt.c_str());
}

void WebSocketsServer::sendTXT(uint8_t num, const char *txt) {
  const int fd = impl_->clientFd(num);
  if (fd >= 0)
    sendWsTextFrame(fd, txt ? txt : "");
}

void WebSocketsServer::close() {
  impl_->active = false;
  if (impl_->fd >= 0) {
    ::shutdown(impl_->fd, SHUT_RDWR);
    ::close(impl_->fd);
    impl_->fd = -1;
  }

  for (const int fd : impl_->clientFds())
    ::shutdown(fd, SHUT_RDWR);
  {
    std::lock_guard<std::mutex> lock(impl_->clientsMutex);
    impl_->clients.clear();
  }

  if (impl_->acceptThread.joinable())
    impl_->acceptThread.join();
  {
    std::lock_guard<std::mutex> lock(impl_->clientThreadsMutex);
    for (auto &thread : impl_->clientThreads) {
      if (thread.joinable())
        thread.join();
    }
    impl_->clientThreads.clear();
  }
  impl_->drainEvents();
}
