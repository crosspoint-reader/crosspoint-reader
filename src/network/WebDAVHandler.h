#pragma once

#include <WebServer.h>

class WebDAVHandler {
 public:
  explicit WebDAVHandler(WebServer* server);

  // Dispatch incoming request by HTTP method. Returns true if handled.
  bool handleRequest();

 private:
  WebServer* _server;

  // WebDAV method handlers
  void handleOptions();
  void handlePropfind();
  void handleGet();
  void handleHead();
  void handlePut();
  void handleDelete();
  void handleMkcol();
  void handleMove();
  void handleCopy();
  void handleLock();
  void handleUnlock();

  // Utilities
  String getRequestPath() const;
  String getDestinationPath() const;
  void urlEncodePath(const String& path, String& out) const;
  bool isProtectedPath(const String& path) const;
  int getDepth() const;
  bool getOverwrite() const;
  void clearEpubCacheIfNeeded(const String& path) const;
  void sendPropEntry(const String& href, bool isDir, size_t size, const String& lastModified) const;
  String getMimeType(const String& path) const;
};
