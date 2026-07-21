#include "WebServer.h"

#include <Logging.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace {
constexpr size_t MAX_BODY_SIZE = 256UL * 1024UL * 1024UL;
constexpr size_t UPLOAD_CHUNK_SIZE = 4096;

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

int hexValue(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

std::string urlDecodeStd(const std::string &input) {
  std::string out;
  out.reserve(input.size());
  for (size_t i = 0; i < input.size(); ++i) {
    if (input[i] == '%' && i + 2 < input.size()) {
      const int hi = hexValue(input[i + 1]);
      const int lo = hexValue(input[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    out.push_back(input[i] == '+' ? ' ' : input[i]);
  }
  return out;
}

const char *reasonForStatus(int status) {
  switch (status) {
  case 200:
    return "OK";
  case 201:
    return "Created";
  case 204:
    return "No Content";
  case 207:
    return "Multi-Status";
  case 400:
    return "Bad Request";
  case 403:
    return "Forbidden";
  case 404:
    return "Not Found";
  case 405:
    return "Method Not Allowed";
  case 409:
    return "Conflict";
  case 412:
    return "Precondition Failed";
  case 413:
    return "Payload Too Large";
  case 415:
    return "Unsupported Media Type";
  case 500:
    return "Internal Server Error";
  case 501:
    return "Not Implemented";
  default:
    return "OK";
  }
}

HTTPMethod methodFromString(const std::string &method) {
  if (method == "GET")
    return HTTP_GET;
  if (method == "POST")
    return HTTP_POST;
  if (method == "PUT")
    return HTTP_PUT;
  if (method == "PATCH")
    return HTTP_PATCH;
  if (method == "DELETE")
    return HTTP_DELETE;
  if (method == "OPTIONS")
    return HTTP_OPTIONS;
  if (method == "PROPFIND")
    return HTTP_PROPFIND;
  if (method == "HEAD")
    return HTTP_HEAD;
  if (method == "MKCOL")
    return HTTP_MKCOL;
  if (method == "MOVE")
    return HTTP_MOVE;
  if (method == "COPY")
    return HTTP_COPY;
  if (method == "LOCK")
    return HTTP_LOCK;
  if (method == "UNLOCK")
    return HTTP_UNLOCK;
  return HTTP_ANY;
}

void parseArgs(const std::string &encoded,
               std::vector<std::pair<String, String>> &args) {
  size_t start = 0;
  while (start <= encoded.size()) {
    const size_t amp = encoded.find('&', start);
    const std::string item = encoded.substr(
        start, amp == std::string::npos ? std::string::npos : amp - start);
    if (!item.empty()) {
      const size_t eq = item.find('=');
      if (eq == std::string::npos) {
        args.emplace_back(String(urlDecodeStd(item)), String(""));
      } else {
        args.emplace_back(String(urlDecodeStd(item.substr(0, eq))),
                          String(urlDecodeStd(item.substr(eq + 1))));
      }
    }
    if (amp == std::string::npos)
      break;
    start = amp + 1;
  }
}

bool sendAll(int client, const void *data, size_t len) {
  const auto *ptr = static_cast<const char *>(data);
#ifdef MSG_NOSIGNAL
  constexpr int sendFlags = MSG_NOSIGNAL;
#else
  constexpr int sendFlags = 0;
#endif
  while (len > 0) {
    const ssize_t written = ::send(client, ptr, len, sendFlags);
    if (written <= 0)
      return false;
    ptr += written;
    len -= static_cast<size_t>(written);
  }
  return true;
}

void sendSimpleResponse(int client, int code, const char *message) {
  const char *body = message ? message : "";
  std::ostringstream out;
  out << "HTTP/1.1 " << code << " " << reasonForStatus(code) << "\r\n"
      << "Content-Length: " << strlen(body) << "\r\n"
      << "Content-Type: text/plain\r\n"
      << "Connection: close\r\n\r\n"
      << body;
  sendAll(client, out.str().data(), out.str().size());
}

void setSocketTimeouts(int client) {
  timeval timeout{};
  timeout.tv_sec = 5;
  setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#ifdef SO_NOSIGPIPE
  int yes = 1;
  setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
}

struct MultipartPart {
  std::string name;
  std::string filename;
  std::string data;
};

std::string headerParam(const std::string &header, const char *key) {
  const std::string needle = std::string(key) + "=\"";
  const size_t start = header.find(needle);
  if (start == std::string::npos)
    return {};
  const size_t valueStart = start + needle.size();
  const size_t valueEnd = header.find('"', valueStart);
  if (valueEnd == std::string::npos)
    return {};
  return header.substr(valueStart, valueEnd - valueStart);
}

std::vector<MultipartPart> parseMultipart(const std::string &contentType,
                                          const std::string &body) {
  std::vector<MultipartPart> parts;
  const size_t boundaryPos = contentType.find("boundary=");
  if (boundaryPos == std::string::npos)
    return parts;

  std::string boundary = contentType.substr(boundaryPos + 9);
  if (!boundary.empty() && boundary.front() == '"') {
    boundary.erase(boundary.begin());
    const size_t quote = boundary.find('"');
    if (quote != std::string::npos)
      boundary.resize(quote);
  } else {
    const size_t semicolon = boundary.find(';');
    if (semicolon != std::string::npos)
      boundary.resize(semicolon);
  }
  boundary = "--" + boundary;

  size_t partStart = body.find(boundary);
  while (partStart != std::string::npos) {
    partStart += boundary.size();
    if (partStart + 2 <= body.size() && body.compare(partStart, 2, "--") == 0)
      break;
    if (partStart + 2 <= body.size() && body.compare(partStart, 2, "\r\n") == 0)
      partStart += 2;

    const size_t headersEnd = body.find("\r\n\r\n", partStart);
    if (headersEnd == std::string::npos)
      break;
    const std::string headers = body.substr(partStart, headersEnd - partStart);
    size_t dataStart = headersEnd + 4;
    size_t next = body.find(boundary, dataStart);
    if (next == std::string::npos)
      break;
    size_t dataEnd = next;
    if (dataEnd >= 2 && body.compare(dataEnd - 2, 2, "\r\n") == 0)
      dataEnd -= 2;

    MultipartPart part;
    part.name = headerParam(headers, "name");
    part.filename = headerParam(headers, "filename");
    part.data = body.substr(dataStart, dataEnd - dataStart);
    if (!part.name.empty())
      parts.push_back(std::move(part));
    partStart = next;
  }
  return parts;
}
} // namespace

struct WebServer::Impl {
  struct Route {
    String uri;
    HTTPMethod method = HTTP_GET;
    std::function<void()> handler;
    std::function<void()> uploadHandler;
  };

  explicit Impl(int serverPort) : port(serverPort == 80 ? 8080 : serverPort) {}

  int port;
  int fd = -1;
  std::atomic<bool> active{false};
  std::thread worker;
  std::vector<Route> routes;
  std::vector<std::unique_ptr<RequestHandler>> requestHandlers;
  std::function<void()> notFoundHandler;
  std::vector<String> collectedHeaders;

  int currentClient = -1;
  HTTPMethod currentMethod = HTTP_GET;
  String currentUri = "/";
  std::vector<std::pair<String, String>> currentArgs;
  std::vector<std::pair<String, String>> currentHeaders;
  std::vector<std::pair<String, String>> pendingHeaders;
  size_t requestContentLength = 0;
  size_t responseContentLength = 0;
  bool responseContentLengthSet = false;
  bool responseContentLengthUnknown = false;
  bool headersSent = false;
  HTTPUpload currentUpload{};

  String argByName(const char *name) const {
    for (const auto &arg : currentArgs) {
      if (arg.first == name)
        return arg.second;
    }
    return String("");
  }

  bool hasArgName(const char *name) const {
    for (const auto &arg : currentArgs) {
      if (arg.first == name)
        return true;
    }
    return false;
  }

  String headerByName(const char *name) const {
    const std::string wanted = lower(name ? name : "");
    for (const auto &header : currentHeaders) {
      if (lower(header.first.s) == wanted)
        return header.second;
    }
    return String("");
  }

  bool hasHeaderName(const char *name) const {
    const std::string wanted = lower(name ? name : "");
    for (const auto &header : currentHeaders) {
      if (lower(header.first.s) == wanted)
        return true;
    }
    return false;
  }

  std::vector<RequestHandler *> rawHandlers(WebServer &server) {
    std::vector<RequestHandler *> out;
    for (auto &requestHandler : requestHandlers) {
      if (!requestHandler->canRaw(server, currentUri))
        continue;
      out.push_back(requestHandler.get());
    }
    return out;
  }

  void emitRawEvent(WebServer &server,
                    const std::vector<RequestHandler *> &handlers,
                    HTTPRawStatus status, uint8_t *buf, size_t currentSize) {
    for (auto *requestHandler : handlers) {
      HTTPRaw rawEvent{};
      rawEvent.status = status;
      rawEvent.totalSize = requestContentLength;
      rawEvent.currentSize = currentSize;
      rawEvent.buf = buf;
      requestHandler->raw(server, currentUri, rawEvent);
    }
  }

  void emitMultipartAbort() {
    for (const auto &route : routes) {
      if (!route.uploadHandler)
        continue;
      if (route.uri != currentUri)
        continue;
      if (!(route.method == currentMethod || route.method == HTTP_ANY))
        continue;
      currentUpload.status = UPLOAD_FILE_ABORTED;
      currentUpload.currentSize = 0;
      currentUpload.totalSize = requestContentLength;
      currentUpload.buf = nullptr;
      route.uploadHandler();
      break;
    }
  }

  void resetRequest() {
    currentClient = -1;
    currentMethod = HTTP_GET;
    currentUri = "/";
    currentArgs.clear();
    currentHeaders.clear();
    pendingHeaders.clear();
    requestContentLength = 0;
    responseContentLength = 0;
    responseContentLengthSet = false;
    responseContentLengthUnknown = false;
    headersSent = false;
    currentUpload = HTTPUpload{};
  }

  void emitHeaders(int code, const char *contentType, size_t bodyLen) {
    if (headersSent || currentClient < 0)
      return;
    std::ostringstream out;
    out << "HTTP/1.1 " << code << " " << reasonForStatus(code) << "\r\n";
    if (responseContentLengthUnknown) {
      out << "Connection: close\r\n";
    } else {
      out << "Content-Length: "
          << (responseContentLengthSet ? responseContentLength : bodyLen)
          << "\r\n";
      out << "Connection: close\r\n";
    }
    out << "Content-Type: " << (contentType ? contentType : "text/plain")
        << "\r\n"
        << "Access-Control-Allow-Origin: *\r\n";
    for (const auto &header : pendingHeaders) {
      out << header.first.c_str() << ": " << header.second.c_str() << "\r\n";
    }
    out << "\r\n";
    sendAll(currentClient, out.str().data(), out.str().size());
    headersSent = true;
  }
};

WebServer::WebServer(int port) : impl_(std::make_unique<Impl>(port)) {}

WebServer::~WebServer() { stop(); }

void WebServer::begin() {
  if (impl_->active)
    return;

  impl_->fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (impl_->fd < 0) {
    LOG_ERR("WEB", "[SIM] WebServer socket failed: %s", strerror(errno));
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
    LOG_ERR("WEB", "[SIM] WebServer bind 127.0.0.1:%d failed: %s", impl_->port,
            strerror(errno));
    ::close(impl_->fd);
    impl_->fd = -1;
    return;
  }
  if (::listen(impl_->fd, 16) != 0) {
    LOG_ERR("WEB", "[SIM] WebServer listen failed: %s", strerror(errno));
    ::close(impl_->fd);
    impl_->fd = -1;
    return;
  }

  impl_->active = true;
  impl_->worker = std::thread([this] {
    while (impl_->active) {
      const int client = ::accept(impl_->fd, nullptr, nullptr);
      if (client < 0) {
        if (impl_->active)
          LOG_ERR("WEB", "[SIM] WebServer accept failed: %s", strerror(errno));
        continue;
      }
      setSocketTimeouts(client);

      std::string raw;
      char buffer[8192];
      while (raw.find("\r\n\r\n") == std::string::npos) {
        const ssize_t got = ::recv(client, buffer, sizeof(buffer), 0);
        if (got <= 0)
          break;
        raw.append(buffer, static_cast<size_t>(got));
        if (raw.size() > 1024 * 1024)
          break;
      }

      const size_t headerEnd = raw.find("\r\n\r\n");
      if (headerEnd == std::string::npos) {
        ::close(client);
        continue;
      }

      std::string body = raw.substr(headerEnd + 4);
      std::istringstream headerStream(raw.substr(0, headerEnd));
      std::string requestLine;
      std::getline(headerStream, requestLine);
      if (!requestLine.empty() && requestLine.back() == '\r')
        requestLine.pop_back();
      std::istringstream requestLineStream(requestLine);
      std::string methodText;
      std::string target;
      requestLineStream >> methodText >> target;

      std::map<std::string, std::string> headers;
      std::string line;
      while (std::getline(headerStream, line)) {
        if (!line.empty() && line.back() == '\r')
          line.pop_back();
        const size_t colon = line.find(':');
        if (colon == std::string::npos)
          continue;
        headers[lower(line.substr(0, colon))] = trim(line.substr(colon + 1));
      }

      size_t bodyLength = 0;
      auto lengthIt = headers.find("content-length");
      if (lengthIt != headers.end())
        bodyLength = static_cast<size_t>(
            std::strtoull(lengthIt->second.c_str(), nullptr, 10));

      const size_t queryStart = target.find('?');
      const std::string path = urlDecodeStd(target.substr(0, queryStart));

      impl_->resetRequest();
      impl_->currentClient = client;
      impl_->currentMethod = methodFromString(methodText);
      impl_->currentUri = path.empty() ? "/" : path.c_str();
      impl_->requestContentLength = bodyLength;
      for (const auto &header : headers) {
        impl_->currentHeaders.emplace_back(String(header.first),
                                           String(header.second));
      }
      if (queryStart != std::string::npos)
        parseArgs(target.substr(queryStart + 1), impl_->currentArgs);

      const auto rawHandlers = impl_->rawHandlers(*this);
      const bool hasRawHandlers = !rawHandlers.empty();
      if (bodyLength > MAX_BODY_SIZE) {
        LOG_ERR("WEB", "[SIM] Request body too large: %zu bytes", bodyLength);
        if (hasRawHandlers) {
          impl_->emitRawEvent(*this, rawHandlers, RAW_START, nullptr, 0);
          impl_->emitRawEvent(*this, rawHandlers, RAW_ABORTED, nullptr, 0);
        }
        sendSimpleResponse(client, 413, "Payload Too Large");
        ::close(client);
        continue;
      }
      if (body.size() > bodyLength)
        body.resize(bodyLength);
      if (hasRawHandlers) {
        impl_->emitRawEvent(*this, rawHandlers, RAW_START, nullptr, 0);
        for (size_t offset = 0; offset < body.size();
             offset += UPLOAD_CHUNK_SIZE) {
          const size_t currentSize =
              std::min(UPLOAD_CHUNK_SIZE, body.size() - offset);
          impl_->emitRawEvent(*this, rawHandlers, RAW_WRITE,
                              reinterpret_cast<uint8_t *>(&body[offset]),
                              currentSize);
        }
      }
      while (body.size() < bodyLength) {
        const ssize_t got = ::recv(client, buffer, sizeof(buffer), 0);
        if (got <= 0)
          break;
        const size_t offset = body.size();
        const size_t wanted = bodyLength - body.size();
        const size_t count = std::min(static_cast<size_t>(got), wanted);
        body.append(buffer, count);
        if (hasRawHandlers) {
          for (size_t rawOffset = offset; rawOffset < body.size();
               rawOffset += UPLOAD_CHUNK_SIZE) {
            const size_t currentSize =
                std::min(UPLOAD_CHUNK_SIZE, body.size() - rawOffset);
            impl_->emitRawEvent(*this, rawHandlers, RAW_WRITE,
                                reinterpret_cast<uint8_t *>(&body[rawOffset]),
                                currentSize);
          }
        }
      }
      if (body.size() < bodyLength) {
        LOG_ERR("WEB", "[SIM] Incomplete request body: %zu/%zu bytes",
                body.size(), bodyLength);
        if (hasRawHandlers)
          impl_->emitRawEvent(*this, rawHandlers, RAW_ABORTED, nullptr, 0);
        auto contentTypeIt = headers.find("content-type");
        const std::string contentType =
            contentTypeIt == headers.end() ? "" : contentTypeIt->second;
        if (contentType.find("multipart/form-data") != std::string::npos)
          impl_->emitMultipartAbort();
        sendSimpleResponse(client, 400, "Incomplete request body");
        ::close(client);
        continue;
      }
      if (hasRawHandlers)
        impl_->emitRawEvent(*this, rawHandlers, RAW_END, nullptr, 0);

      auto contentTypeIt = headers.find("content-type");
      const std::string contentType =
          contentTypeIt == headers.end() ? "" : contentTypeIt->second;
      if (contentType.find("application/x-www-form-urlencoded") !=
          std::string::npos) {
        parseArgs(body, impl_->currentArgs);
      } else if (!body.empty() &&
                 contentType.find("multipart/form-data") == std::string::npos) {
        impl_->currentArgs.emplace_back(String("plain"), String(body));
      }

      LOG_DBG("WEB", "[SIM] %s %s", methodText.c_str(), target.c_str());

      bool handled = false;
      for (const auto &route : impl_->routes) {
        const bool headMatchesGet =
            impl_->currentMethod == HTTP_HEAD && route.method == HTTP_GET;
        if (route.uri == impl_->currentUri &&
            (route.method == impl_->currentMethod || route.method == HTTP_ANY ||
             headMatchesGet)) {
          if (route.uploadHandler &&
              contentType.find("multipart/form-data") != std::string::npos) {
            const auto parts = parseMultipart(contentType, body);
            for (const auto &part : parts) {
              if (part.filename.empty()) {
                impl_->currentArgs.emplace_back(String(part.name),
                                                String(part.data));
              }
            }
            for (const auto &part : parts) {
              if (part.filename.empty())
                continue;
              impl_->currentUpload.name = part.name.c_str();
              impl_->currentUpload.filename = part.filename.c_str();
              impl_->currentUpload.totalSize = part.data.size();
              impl_->currentUpload.status = UPLOAD_FILE_START;
              impl_->currentUpload.currentSize = 0;
              impl_->currentUpload.buf = nullptr;
              route.uploadHandler();
              for (size_t offset = 0; offset < part.data.size();
                   offset += UPLOAD_CHUNK_SIZE) {
                impl_->currentUpload.status = UPLOAD_FILE_WRITE;
                impl_->currentUpload.currentSize =
                    std::min(UPLOAD_CHUNK_SIZE, part.data.size() - offset);
                impl_->currentUpload.buf = reinterpret_cast<uint8_t *>(
                    const_cast<char *>(part.data.data() + offset));
                route.uploadHandler();
              }
              impl_->currentUpload.status = UPLOAD_FILE_END;
              impl_->currentUpload.currentSize = 0;
              impl_->currentUpload.buf = nullptr;
              route.uploadHandler();
            }
          }
          route.handler();
          handled = true;
          break;
        }
      }

      if (!handled) {
        for (auto &requestHandler : impl_->requestHandlers) {
          if (requestHandler->canHandle(*this, impl_->currentMethod,
                                        impl_->currentUri) &&
              requestHandler->handle(*this, impl_->currentMethod,
                                     impl_->currentUri)) {
            handled = true;
            break;
          }
        }
      }

      if (!handled) {
        if (impl_->notFoundHandler) {
          impl_->notFoundHandler();
        } else {
          send(404, "text/plain", "Not Found");
        }
      }

      ::close(client);
      impl_->resetRequest();
    }
  });
  LOG_DBG("WEB", "[SIM] WebServer running at http://127.0.0.1:%d/",
          impl_->port);
}

void WebServer::handleClient() {}

void WebServer::on(const char *uri, int method, std::function<void()> handler) {
  impl_->routes.push_back({String(uri), static_cast<HTTPMethod>(method),
                           std::move(handler), nullptr});
}

void WebServer::on(const char *uri, int method, std::function<void()> handler,
                   std::function<void()> uploadHandler) {
  impl_->routes.push_back({String(uri), static_cast<HTTPMethod>(method),
                           std::move(handler), std::move(uploadHandler)});
}

void WebServer::onNotFound(std::function<void()> handler) {
  impl_->notFoundHandler = std::move(handler);
}

void WebServer::collectHeaders(const char **headers, size_t count) {
  impl_->collectedHeaders.clear();
  for (size_t i = 0; i < count; ++i) {
    if (headers[i])
      impl_->collectedHeaders.emplace_back(headers[i]);
  }
}

void WebServer::stop() {
  impl_->active = false;
  if (impl_->fd >= 0) {
    ::shutdown(impl_->fd, SHUT_RDWR);
    ::close(impl_->fd);
    impl_->fd = -1;
  }
  if (impl_->worker.joinable())
    impl_->worker.join();
}

void WebServer::addHandler(RequestHandler *handler) {
  impl_->requestHandlers.emplace_back(handler);
}

void WebServer::send(int code, const char *content_type, const char *content) {
  const char *body = content ? content : "";
  const size_t bodyLen = strlen(body);
  impl_->emitHeaders(code, content_type, bodyLen);
  if (impl_->currentMethod != HTTP_HEAD && bodyLen > 0)
    sendAll(impl_->currentClient, body, bodyLen);
}

void WebServer::send_P(int code, const char *content_type, const char *content,
                       size_t len) {
  impl_->emitHeaders(code, content_type, len);
  if (impl_->currentMethod != HTTP_HEAD && content && len > 0)
    sendAll(impl_->currentClient, content, len);
}

void WebServer::sendHeader(const char *name, const char *value, bool first) {
  if (!name)
    return;
  if (first) {
    impl_->pendingHeaders.insert(impl_->pendingHeaders.begin(),
                                 {String(name), String(value ? value : "")});
  } else {
    impl_->pendingHeaders.emplace_back(String(name),
                                       String(value ? value : ""));
  }
}

void WebServer::sendContent(const String &content) {
  sendContent(content.c_str());
}

void WebServer::sendContent(const char *content) {
  const char *body = content ? content : "";
  if (!impl_->headersSent)
    impl_->emitHeaders(200, "text/plain", strlen(body));
  if (impl_->currentMethod != HTTP_HEAD && body[0] != '\0')
    sendAll(impl_->currentClient, body, strlen(body));
}

void WebServer::setContentLength(size_t len) {
  impl_->responseContentLengthUnknown =
      (static_cast<int>(len) == CONTENT_LENGTH_UNKNOWN);
  impl_->responseContentLengthSet = !impl_->responseContentLengthUnknown;
  if (impl_->responseContentLengthSet)
    impl_->responseContentLength = len;
}

int WebServer::method() { return impl_->currentMethod; }
String WebServer::uri() { return impl_->currentUri; }
bool WebServer::hasArg(const char *name) { return impl_->hasArgName(name); }
String WebServer::arg(const char *name) { return impl_->argByName(name); }
String WebServer::arg(int i) {
  return i >= 0 && i < static_cast<int>(impl_->currentArgs.size())
             ? impl_->currentArgs[i].second
             : String("");
}
int WebServer::args() { return static_cast<int>(impl_->currentArgs.size()); }
String WebServer::argName(int i) {
  return i >= 0 && i < static_cast<int>(impl_->currentArgs.size())
             ? impl_->currentArgs[i].first
             : String("");
}
String WebServer::header(const char *name) { return impl_->headerByName(name); }
String WebServer::header(int i) {
  return i >= 0 && i < static_cast<int>(impl_->currentHeaders.size())
             ? impl_->currentHeaders[i].second
             : String("");
}
String WebServer::headerName(int i) {
  return i >= 0 && i < static_cast<int>(impl_->currentHeaders.size())
             ? impl_->currentHeaders[i].first
             : String("");
}
int WebServer::headers() {
  return static_cast<int>(impl_->currentHeaders.size());
}
bool WebServer::hasHeader(const char *name) {
  return impl_->hasHeaderName(name);
}
String WebServer::urlDecode(const String &str) {
  return String(urlDecodeStd(str.s));
}
NetworkClient WebServer::client() {
  if (impl_->currentClient < 0)
    return NetworkClient(-1);
  const int fd = ::dup(impl_->currentClient);
  if (fd < 0)
    LOG_ERR("WEB", "[SIM] Failed to duplicate client socket: %s",
            strerror(errno));
  return NetworkClient(fd);
}
long WebServer::clientContentLength() {
  return static_cast<long>(impl_->requestContentLength);
}
HTTPUpload &WebServer::upload() { return impl_->currentUpload; }
