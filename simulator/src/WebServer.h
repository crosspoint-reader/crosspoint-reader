#pragma once
#include <functional>
#include <memory>

#include "NetworkClient.h"
#include "WString.h"

enum HTTPMethod {
  HTTP_GET,
  HTTP_POST,
  HTTP_PUT,
  HTTP_PATCH,
  HTTP_DELETE,
  HTTP_OPTIONS,
  HTTP_PROPFIND,
  HTTP_HEAD,
  HTTP_MKCOL,
  HTTP_MOVE,
  HTTP_COPY,
  HTTP_LOCK,
  HTTP_UNLOCK,
  HTTP_ANY
};

enum HTTPRawStatus { RAW_START, RAW_WRITE, RAW_END, RAW_ABORTED };

struct HTTPRaw {
  HTTPRawStatus status;
  uint8_t *buf;
  size_t currentSize;
  size_t totalSize;
};

enum HTTPUploadStatus {
  UPLOAD_FILE_START,
  UPLOAD_FILE_WRITE,
  UPLOAD_FILE_END,
  UPLOAD_FILE_ABORTED
};

struct HTTPUpload {
  String filename;
  String name;
  HTTPUploadStatus status;
  size_t totalSize;
  size_t currentSize;
  uint8_t *buf;
};

const int CONTENT_LENGTH_UNKNOWN = -1;

class WebServer;

class RequestHandler {
public:
  virtual ~RequestHandler() {}
  virtual bool canHandle(WebServer &request, HTTPMethod method,
                         const String &uri) {
    return false;
  }
  virtual bool canRaw(WebServer &request, const String &uri) { return false; }
  virtual void raw(WebServer &request, const String &uri, HTTPRaw &raw) {}
  virtual bool handle(WebServer &request, HTTPMethod method,
                      const String &uri) {
    return false;
  }
};

class WebServer {
public:
  WebServer(int port);
  ~WebServer();
  void begin();
  void handleClient();
  void on(const char *uri, int method, std::function<void()> handler);
  void on(const char *uri, int method, std::function<void()> handler,
          std::function<void()> uploadHandler);
  void onNotFound(std::function<void()> handler);
  void collectHeaders(const char **headers, size_t count);
  void stop();
  void addHandler(RequestHandler *handler);
  void send(int code, const char *content_type, const char *content);
  void send(int code, const char *content_type, const String &content) {
    send(code, content_type, content.c_str());
  }
  void send(int code) { send(code, "text/plain", ""); }
  void send_P(int code, const char *content_type, const char *content,
              size_t len);
  void sendHeader(const char *name, const char *value, bool first = false);
  void sendHeader(const char *name, const String &value, bool first = false) {
    sendHeader(name, value.c_str(), first);
  }
  void sendContent(const String &content);
  void sendContent(const char *content);
  void setContentLength(size_t len);
  int method();
  String uri();
  bool hasArg(const char *name);
  String arg(const char *name);
  String arg(int i);
  int args();
  String argName(int i);
  String header(const char *name);
  String header(int i);
  String headerName(int i);
  int headers();
  bool hasHeader(const char *name);
  static String urlDecode(const String &str);
  NetworkClient client();
  long clientContentLength();
  HTTPUpload &upload();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
