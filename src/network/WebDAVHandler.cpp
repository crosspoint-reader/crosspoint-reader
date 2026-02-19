#include "WebDAVHandler.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_task_wdt.h>

#include "util/StringUtils.h"

namespace {
const char* HIDDEN_ITEMS[] = {"System Volume Information", "XTCache"};
constexpr size_t HIDDEN_ITEMS_COUNT = sizeof(HIDDEN_ITEMS) / sizeof(HIDDEN_ITEMS[0]);

// RFC 1123 date format helper: "Sun, 06 Nov 1994 08:49:37 GMT"
// ESP32 doesn't have real-time clock set by default, so we use a fixed epoch date
// as a fallback. The date is not critical for WebDAV Class 1 operations.
const char* FIXED_DATE = "Thu, 01 Jan 2024 00:00:00 GMT";
}  // namespace

WebDAVHandler::WebDAVHandler(WebServer* server) : _server(server) {}

bool WebDAVHandler::handleRequest() {
  HTTPMethod method = _server->method();

  switch (method) {
    case HTTP_OPTIONS:
      handleOptions();
      return true;
    case HTTP_PROPFIND:
      handlePropfind();
      return true;
    case HTTP_GET:
      handleGet();
      return true;
    case HTTP_HEAD:
      handleHead();
      return true;
    case HTTP_PUT:
      handlePut();
      return true;
    case HTTP_DELETE:
      handleDelete();
      return true;
    case HTTP_MKCOL:
      handleMkcol();
      return true;
    case HTTP_MOVE:
      handleMove();
      return true;
    case HTTP_COPY:
      handleCopy();
      return true;
    case HTTP_LOCK:
      handleLock();
      return true;
    case HTTP_UNLOCK:
      handleUnlock();
      return true;
    default:
      return false;
  }
}

// ── OPTIONS ──────────────────────────────────────────────────────────────────

void WebDAVHandler::handleOptions() {
  _server->sendHeader("DAV", "1");
  _server->sendHeader("Allow",
                       "OPTIONS, GET, HEAD, PUT, DELETE, "
                       "PROPFIND, MKCOL, MOVE, COPY, LOCK, UNLOCK");
  _server->sendHeader("MS-Author-Via", "DAV");
  _server->send(200);
  LOG_DBG("DAV", "OPTIONS %s", _server->uri().c_str());
}

// ── PROPFIND ─────────────────────────────────────────────────────────────────

void WebDAVHandler::handlePropfind() {
  String path = getRequestPath();
  int depth = getDepth();

  LOG_DBG("DAV", "PROPFIND %s depth=%d", path.c_str(), depth);

  // Check if path exists
  if (!Storage.exists(path.c_str()) && path != "/") {
    _server->send(404, "text/plain", "Not Found");
    return;
  }

  FsFile root = Storage.open(path.c_str());
  if (!root) {
    if (path == "/") {
      // Root should always work — send minimal response
      _server->setContentLength(CONTENT_LENGTH_UNKNOWN);
      _server->send(207, "application/xml; charset=\"utf-8\"", "");
      _server->sendContent("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                           "<D:multistatus xmlns:D=\"DAV:\">\n");
      sendPropEntry("/", true, 0, FIXED_DATE);
      _server->sendContent("</D:multistatus>\n");
      _server->sendContent("");
      return;
    }
    _server->send(500, "text/plain", "Failed to open");
    return;
  }

  bool isDir = root.isDirectory();

  _server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  _server->send(207, "application/xml; charset=\"utf-8\"", "");
  _server->sendContent("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                       "<D:multistatus xmlns:D=\"DAV:\">\n");

  // Entry for the resource itself
  if (isDir) {
    sendPropEntry(path, true, 0, FIXED_DATE);
  } else {
    sendPropEntry(path, false, root.size(), FIXED_DATE);
    root.close();
    _server->sendContent("</D:multistatus>\n");
    _server->sendContent("");
    return;
  }

  // If depth > 0 and it's a directory, list children
  if (depth > 0) {
    FsFile file = root.openNextFile();
    char name[500];
    while (file) {
      file.getName(name, sizeof(name));
      String fileName(name);

      // Skip hidden/protected items
      bool shouldHide = fileName.startsWith(".");
      if (!shouldHide) {
        for (size_t i = 0; i < HIDDEN_ITEMS_COUNT; i++) {
          if (fileName.equals(HIDDEN_ITEMS[i])) {
            shouldHide = true;
            break;
          }
        }
      }

      if (!shouldHide) {
        String childPath = path;
        if (!childPath.endsWith("/")) childPath += "/";
        childPath += fileName;

        if (file.isDirectory()) {
          sendPropEntry(childPath, true, 0, FIXED_DATE);
        } else {
          sendPropEntry(childPath, false, file.size(), FIXED_DATE);
        }
      }

      file.close();
      yield();
      esp_task_wdt_reset();
      file = root.openNextFile();
    }
  }

  root.close();
  _server->sendContent("</D:multistatus>\n");
  _server->sendContent("");
}

void WebDAVHandler::sendPropEntry(const String& path, bool isDir, size_t size,
                                  const String& lastModified) const {
  String href;
  urlEncodePath(path, href);
  // Ensure directory hrefs end with /
  if (isDir && !href.endsWith("/")) href += "/";

  String xml = "<D:response><D:href>";
  xml += href;
  xml += "</D:href><D:propstat><D:prop>";

  if (isDir) {
    xml += "<D:resourcetype><D:collection/></D:resourcetype>";
  } else {
    xml += "<D:resourcetype/>";
    xml += "<D:getcontentlength>";
    xml += String(size);
    xml += "</D:getcontentlength>";
    String mime = getMimeType(path);
    xml += "<D:getcontenttype>";
    xml += mime;
    xml += "</D:getcontenttype>";
  }

  xml += "<D:getlastmodified>";
  xml += lastModified;
  xml += "</D:getlastmodified>";

  xml += "</D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>\n";

  _server->sendContent(xml);
}

// ── GET ──────────────────────────────────────────────────────────────────────

void WebDAVHandler::handleGet() {
  String path = getRequestPath();
  LOG_DBG("DAV", "GET %s", path.c_str());

  if (isProtectedPath(path)) {
    _server->send(403, "text/plain", "Forbidden");
    return;
  }

  if (!Storage.exists(path.c_str())) {
    _server->send(404, "text/plain", "Not Found");
    return;
  }

  FsFile file = Storage.open(path.c_str());
  if (!file) {
    _server->send(500, "text/plain", "Failed to open file");
    return;
  }
  if (file.isDirectory()) {
    file.close();
    // For directories, return a PROPFIND-like response or redirect
    _server->send(405, "text/plain", "Method Not Allowed");
    return;
  }

  String contentType = getMimeType(path);
  _server->setContentLength(file.size());
  _server->send(200, contentType.c_str(), "");

  WiFiClient client = _server->client();
  client.write(file);
  file.close();
}

// ── HEAD ─────────────────────────────────────────────────────────────────────

void WebDAVHandler::handleHead() {
  String path = getRequestPath();
  LOG_DBG("DAV", "HEAD %s", path.c_str());

  if (isProtectedPath(path)) {
    _server->send(403, "text/plain", "");
    return;
  }

  if (!Storage.exists(path.c_str())) {
    _server->send(404, "text/plain", "");
    return;
  }

  FsFile file = Storage.open(path.c_str());
  if (!file) {
    _server->send(500, "text/plain", "");
    return;
  }

  if (file.isDirectory()) {
    file.close();
    _server->send(200, "text/html", "");
    return;
  }

  String contentType = getMimeType(path);
  _server->setContentLength(file.size());
  _server->send(200, contentType.c_str(), "");
  file.close();
}

// ── PUT ──────────────────────────────────────────────────────────────────────

void WebDAVHandler::handlePut() {
  String path = getRequestPath();
  LOG_DBG("DAV", "PUT %s", path.c_str());

  if (isProtectedPath(path)) {
    _server->send(403, "text/plain", "Forbidden");
    return;
  }

  // Ensure parent directory exists
  int lastSlash = path.lastIndexOf('/');
  if (lastSlash > 0) {
    String parentPath = path.substring(0, lastSlash);
    if (!Storage.exists(parentPath.c_str())) {
      _server->send(409, "text/plain", "Parent directory does not exist");
      return;
    }
  }

  WiFiClient client = _server->client();
  int contentLength = _server->clientContentLength();

  bool existed = Storage.exists(path.c_str());

  // Remove existing file if overwriting
  if (existed) {
    FsFile existing = Storage.open(path.c_str());
    if (existing) {
      if (existing.isDirectory()) {
        existing.close();
        _server->send(409, "text/plain", "Cannot overwrite directory with file");
        return;
      }
      existing.close();
    }
    Storage.remove(path.c_str());
  }

  FsFile file;
  if (!Storage.openFileForWrite("DAV", path, file)) {
    _server->send(500, "text/plain", "Failed to create file");
    return;
  }

  if (contentLength > 0) {
    uint8_t buf[4096];
    size_t remaining = contentLength;
    while (remaining > 0 && client.connected()) {
      esp_task_wdt_reset();
      size_t toRead = remaining < sizeof(buf) ? remaining : sizeof(buf);
      size_t bytesRead = client.readBytes(buf, toRead);
      if (bytesRead == 0) break;
      file.write(buf, bytesRead);
      remaining -= bytesRead;
    }
  }

  file.close();
  clearEpubCacheIfNeeded(path);

  _server->send(existed ? 204 : 201);
  LOG_DBG("DAV", "PUT complete: %s (%d bytes)", path.c_str(), contentLength);
}

// ── DELETE ───────────────────────────────────────────────────────────────────

void WebDAVHandler::handleDelete() {
  String path = getRequestPath();
  LOG_DBG("DAV", "DELETE %s", path.c_str());

  if (path == "/" || path.isEmpty()) {
    _server->send(403, "text/plain", "Cannot delete root");
    return;
  }

  if (isProtectedPath(path)) {
    _server->send(403, "text/plain", "Forbidden");
    return;
  }

  if (!Storage.exists(path.c_str())) {
    _server->send(404, "text/plain", "Not Found");
    return;
  }

  FsFile file = Storage.open(path.c_str());
  if (!file) {
    _server->send(500, "text/plain", "Failed to open");
    return;
  }

  if (file.isDirectory()) {
    // Check if directory is empty
    FsFile entry = file.openNextFile();
    if (entry) {
      entry.close();
      file.close();
      _server->send(409, "text/plain", "Directory not empty");
      return;
    }
    file.close();
    if (Storage.rmdir(path.c_str())) {
      _server->send(204);
    } else {
      _server->send(500, "text/plain", "Failed to remove directory");
    }
  } else {
    file.close();
    clearEpubCacheIfNeeded(path);
    if (Storage.remove(path.c_str())) {
      _server->send(204);
    } else {
      _server->send(500, "text/plain", "Failed to delete file");
    }
  }
}

// ── MKCOL ────────────────────────────────────────────────────────────────────

void WebDAVHandler::handleMkcol() {
  String path = getRequestPath();
  LOG_DBG("DAV", "MKCOL %s", path.c_str());

  if (isProtectedPath(path)) {
    _server->send(403, "text/plain", "Forbidden");
    return;
  }

  // MKCOL must not have a body (RFC 4918)
  if (_server->clientContentLength() > 0) {
    _server->send(415, "text/plain", "Unsupported Media Type");
    return;
  }

  if (Storage.exists(path.c_str())) {
    _server->send(405, "text/plain", "Already exists");
    return;
  }

  // Check parent exists
  int lastSlash = path.lastIndexOf('/');
  if (lastSlash > 0) {
    String parentPath = path.substring(0, lastSlash);
    if (!parentPath.isEmpty() && !Storage.exists(parentPath.c_str())) {
      _server->send(409, "text/plain", "Parent directory does not exist");
      return;
    }
  }

  if (Storage.mkdir(path.c_str())) {
    _server->send(201);
    LOG_DBG("DAV", "Created directory: %s", path.c_str());
  } else {
    _server->send(500, "text/plain", "Failed to create directory");
  }
}

// ── MOVE ─────────────────────────────────────────────────────────────────────

void WebDAVHandler::handleMove() {
  String srcPath = getRequestPath();
  String dstPath = getDestinationPath();
  bool overwrite = getOverwrite();

  LOG_DBG("DAV", "MOVE %s -> %s (overwrite=%d)", srcPath.c_str(), dstPath.c_str(), overwrite);

  if (srcPath == "/" || srcPath.isEmpty()) {
    _server->send(403, "text/plain", "Cannot move root");
    return;
  }

  if (isProtectedPath(srcPath) || isProtectedPath(dstPath)) {
    _server->send(403, "text/plain", "Forbidden");
    return;
  }

  if (dstPath.isEmpty()) {
    _server->send(400, "text/plain", "Missing Destination header");
    return;
  }

  if (!Storage.exists(srcPath.c_str())) {
    _server->send(404, "text/plain", "Source not found");
    return;
  }

  // Check destination parent exists
  int lastSlash = dstPath.lastIndexOf('/');
  if (lastSlash > 0) {
    String parentPath = dstPath.substring(0, lastSlash);
    if (!parentPath.isEmpty() && !Storage.exists(parentPath.c_str())) {
      _server->send(409, "text/plain", "Destination parent does not exist");
      return;
    }
  }

  bool dstExists = Storage.exists(dstPath.c_str());
  if (dstExists && !overwrite) {
    _server->send(412, "text/plain", "Destination exists and Overwrite is F");
    return;
  }

  if (dstExists) {
    Storage.remove(dstPath.c_str());
  }

  FsFile file = Storage.open(srcPath.c_str());
  if (!file) {
    _server->send(500, "text/plain", "Failed to open source");
    return;
  }

  clearEpubCacheIfNeeded(srcPath);
  bool success = file.rename(dstPath.c_str());
  file.close();

  if (success) {
    _server->send(dstExists ? 204 : 201);
  } else {
    _server->send(500, "text/plain", "Move failed");
  }
}

// ── COPY ─────────────────────────────────────────────────────────────────────

void WebDAVHandler::handleCopy() {
  String srcPath = getRequestPath();
  String dstPath = getDestinationPath();
  bool overwrite = getOverwrite();

  LOG_DBG("DAV", "COPY %s -> %s (overwrite=%d)", srcPath.c_str(), dstPath.c_str(), overwrite);

  if (isProtectedPath(srcPath) || isProtectedPath(dstPath)) {
    _server->send(403, "text/plain", "Forbidden");
    return;
  }

  if (dstPath.isEmpty()) {
    _server->send(400, "text/plain", "Missing Destination header");
    return;
  }

  if (!Storage.exists(srcPath.c_str())) {
    _server->send(404, "text/plain", "Source not found");
    return;
  }

  FsFile srcFile = Storage.open(srcPath.c_str());
  if (!srcFile) {
    _server->send(500, "text/plain", "Failed to open source");
    return;
  }

  if (srcFile.isDirectory()) {
    srcFile.close();
    _server->send(403, "text/plain", "Cannot copy directories");
    return;
  }

  // Check destination parent exists
  int lastSlash = dstPath.lastIndexOf('/');
  if (lastSlash > 0) {
    String parentPath = dstPath.substring(0, lastSlash);
    if (!parentPath.isEmpty() && !Storage.exists(parentPath.c_str())) {
      srcFile.close();
      _server->send(409, "text/plain", "Destination parent does not exist");
      return;
    }
  }

  bool dstExists = Storage.exists(dstPath.c_str());
  if (dstExists && !overwrite) {
    srcFile.close();
    _server->send(412, "text/plain", "Destination exists and Overwrite is F");
    return;
  }

  if (dstExists) {
    Storage.remove(dstPath.c_str());
  }

  FsFile dstFile;
  if (!Storage.openFileForWrite("DAV", dstPath, dstFile)) {
    srcFile.close();
    _server->send(500, "text/plain", "Failed to create destination");
    return;
  }

  // Streaming copy with 4KB buffer on stack
  uint8_t buf[4096];
  bool copyOk = true;
  while (srcFile.available()) {
    esp_task_wdt_reset();
    int bytesRead = srcFile.read(buf, sizeof(buf));
    if (bytesRead <= 0) break;
    size_t written = dstFile.write(buf, bytesRead);
    if (written != (size_t)bytesRead) {
      copyOk = false;
      break;
    }
  }

  srcFile.close();
  dstFile.close();

  if (copyOk) {
    _server->send(dstExists ? 204 : 201);
  } else {
    Storage.remove(dstPath.c_str());
    _server->send(500, "text/plain", "Copy failed - disk full?");
  }
}

// ── LOCK / UNLOCK (dummy for client compatibility) ───────────────────────────

void WebDAVHandler::handleLock() {
  String path = getRequestPath();
  LOG_DBG("DAV", "LOCK %s (dummy)", path.c_str());

  // Return a dummy lock token for client compatibility
  String xml =
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
      "<D:prop xmlns:D=\"DAV:\">\n"
      "<D:lockdiscovery><D:activelock>\n"
      "<D:locktype><D:write/></D:locktype>\n"
      "<D:lockscope><D:exclusive/></D:lockscope>\n"
      "<D:depth>infinity</D:depth>\n"
      "<D:owner><D:href>crosspoint</D:href></D:owner>\n"
      "<D:timeout>Second-3600</D:timeout>\n"
      "<D:locktoken><D:href>urn:uuid:dummy-lock-token</D:href></D:locktoken>\n"
      "<D:lockroot><D:href>/</D:href></D:lockroot>\n"
      "</D:activelock></D:lockdiscovery>\n"
      "</D:prop>\n";

  _server->sendHeader("Lock-Token", "<urn:uuid:dummy-lock-token>");
  _server->send(200, "application/xml; charset=\"utf-8\"", xml);
}

void WebDAVHandler::handleUnlock() {
  LOG_DBG("DAV", "UNLOCK %s (dummy)", _server->uri().c_str());
  _server->send(204);
}

// ── Utility functions ────────────────────────────────────────────────────────

String WebDAVHandler::getRequestPath() const {
  String uri = _server->uri();
  String decoded = WebServer::urlDecode(uri);

  // Normalize using FsHelpers
  std::string normalized = FsHelpers::normalisePath(decoded.c_str());
  String result = normalized.c_str();

  if (result.isEmpty()) return "/";
  if (!result.startsWith("/")) result = "/" + result;

  // Remove trailing slash unless root
  if (result.length() > 1 && result.endsWith("/")) {
    result = result.substring(0, result.length() - 1);
  }

  return result;
}

String WebDAVHandler::getDestinationPath() const {
  String dest = _server->header("Destination");
  if (dest.isEmpty()) return "";

  // Extract path from full URL: http://host/path -> /path
  // Find the third slash (after http://)
  int schemeEnd = dest.indexOf("://");
  if (schemeEnd >= 0) {
    int pathStart = dest.indexOf('/', schemeEnd + 3);
    if (pathStart >= 0) {
      dest = dest.substring(pathStart);
    } else {
      dest = "/";
    }
  }

  String decoded = WebServer::urlDecode(dest);
  std::string normalized = FsHelpers::normalisePath(decoded.c_str());
  String result = normalized.c_str();

  if (result.isEmpty()) return "/";
  if (!result.startsWith("/")) result = "/" + result;

  // Remove trailing slash unless root
  if (result.length() > 1 && result.endsWith("/")) {
    result = result.substring(0, result.length() - 1);
  }

  return result;
}

void WebDAVHandler::urlEncodePath(const String& path, String& out) const {
  out = "";
  for (unsigned int i = 0; i < path.length(); i++) {
    char c = path.charAt(i);
    if (c == '/') {
      out += '/';
    } else if (c == ' ') {
      out += "%20";
    } else if (c == '%') {
      out += "%25";
    } else if (c == '#') {
      out += "%23";
    } else if (c == '?') {
      out += "%3F";
    } else if (c == '&') {
      out += "%26";
    } else if ((uint8_t)c > 127) {
      // Encode non-ASCII bytes
      char hex[4];
      snprintf(hex, sizeof(hex), "%%%02X", (uint8_t)c);
      out += hex;
    } else {
      out += c;
    }
  }
}

bool WebDAVHandler::isProtectedPath(const String& path) const {
  // Extract the filename/dirname component
  String name = path;
  int lastSlash = path.lastIndexOf('/');
  if (lastSlash >= 0) {
    name = path.substring(lastSlash + 1);
  }
  if (name.isEmpty()) return false;

  if (name.startsWith(".")) return true;

  for (size_t i = 0; i < HIDDEN_ITEMS_COUNT; i++) {
    if (name.equals(HIDDEN_ITEMS[i])) return true;
  }

  return false;
}

int WebDAVHandler::getDepth() const {
  String depth = _server->header("Depth");
  if (depth == "0") return 0;
  if (depth == "1") return 1;
  // "infinity" or missing → treat as 1 (Class 1 servers don't need to support infinity)
  return 1;
}

bool WebDAVHandler::getOverwrite() const {
  String ow = _server->header("Overwrite");
  if (ow == "F" || ow == "f") return false;
  return true;  // Default is T
}

void WebDAVHandler::clearEpubCacheIfNeeded(const String& path) const {
  if (StringUtils::checkFileExtension(path, ".epub")) {
    Epub(path.c_str(), "/.crosspoint").clearCache();
    LOG_DBG("DAV", "Cleared epub cache for: %s", path.c_str());
  }
}

String WebDAVHandler::getMimeType(const String& path) const {
  if (StringUtils::checkFileExtension(path, ".epub")) return "application/epub+zip";
  if (StringUtils::checkFileExtension(path, ".pdf")) return "application/pdf";
  if (StringUtils::checkFileExtension(path, ".txt")) return "text/plain";
  if (StringUtils::checkFileExtension(path, ".html") || StringUtils::checkFileExtension(path, ".htm"))
    return "text/html";
  if (StringUtils::checkFileExtension(path, ".css")) return "text/css";
  if (StringUtils::checkFileExtension(path, ".js")) return "application/javascript";
  if (StringUtils::checkFileExtension(path, ".json")) return "application/json";
  if (StringUtils::checkFileExtension(path, ".xml")) return "application/xml";
  if (StringUtils::checkFileExtension(path, ".jpg") || StringUtils::checkFileExtension(path, ".jpeg"))
    return "image/jpeg";
  if (StringUtils::checkFileExtension(path, ".png")) return "image/png";
  if (StringUtils::checkFileExtension(path, ".gif")) return "image/gif";
  if (StringUtils::checkFileExtension(path, ".svg")) return "image/svg+xml";
  if (StringUtils::checkFileExtension(path, ".zip")) return "application/zip";
  if (StringUtils::checkFileExtension(path, ".gz")) return "application/gzip";
  return "application/octet-stream";
}
