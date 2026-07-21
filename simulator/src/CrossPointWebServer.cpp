#ifndef CROSSPOINT_SIMULATOR_PROJECT_WEBSERVER

#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

#if __has_include(<AppVersion.h>)
#include <AppVersion.h>
#endif

#include "network/CrossPointWebServer.h"

#ifndef CROSSPOINT_VERSION
#ifdef CROSSINK_VERSION
#define CROSSPOINT_VERSION CROSSINK_VERSION
#else
#define CROSSPOINT_VERSION "dev-simulator"
#endif
#endif

namespace {
constexpr int SIMULATOR_WEB_PORT = 8080;
constexpr size_t MAX_BODY_SIZE = 256UL * 1024UL * 1024UL;

struct Request {
  std::string method;
  std::string target;
  std::string path;
  std::map<std::string, std::string> query;
  std::unordered_map<std::string, std::string> headers;
  std::string body;
};

struct NativeServerState {
  std::atomic<bool> active{false};
  int fd = -1;
  std::thread worker;
  mutable std::mutex uploadMutex;
  CrossPointWebServer::WsUploadStatus uploadStatus;
};

std::mutex statesMutex;
std::map<const CrossPointWebServer *, std::unique_ptr<NativeServerState>>
    states;

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

std::string urlDecode(const std::string &input) {
  std::string out;
  out.reserve(input.size());
  for (size_t i = 0; i < input.size(); ++i) {
    if (input[i] == '%' && i + 2 < input.size()) {
      const std::string hex = input.substr(i + 1, 2);
      char *end = nullptr;
      long value = std::strtol(hex.c_str(), &end, 16);
      if (end && *end == '\0') {
        out.push_back(static_cast<char>(value));
        i += 2;
        continue;
      }
    }
    out.push_back(input[i] == '+' ? ' ' : input[i]);
  }
  return out;
}

std::string jsonEscape(const std::string &input) {
  std::string out;
  out.reserve(input.size() + 8);
  for (char c : input) {
    switch (c) {
    case '\\':
      out += "\\\\";
      break;
    case '"':
      out += "\\\"";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        char buf[7];
        snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
        out += buf;
      } else {
        out.push_back(c);
      }
      break;
    }
  }
  return out;
}

std::string normalizePath(std::string path) {
  path = urlDecode(path);
  if (path.empty())
    path = "/";
  if (path.front() != '/')
    path.insert(path.begin(), '/');
  while (path.find("//") != std::string::npos) {
    path.replace(path.find("//"), 2, "/");
  }
  std::stringstream parts(path);
  std::string part;
  while (std::getline(parts, part, '/')) {
    if (part == "..")
      return {};
  }
  if (path.size() > 1 && path.back() == '/')
    path.pop_back();
  return path;
}

bool isProtectedName(const std::string &name) {
  return name.empty() || name[0] == '.' ||
         name == "System Volume Information" || name == "XTCache";
}

bool isProtectedPath(const std::string &path) {
  if (path.empty())
    return true;
  size_t start = 0;
  while (start < path.size()) {
    while (start < path.size() && path[start] == '/')
      start++;
    size_t end = path.find('/', start);
    const std::string part = path.substr(
        start, end == std::string::npos ? std::string::npos : end - start);
    if (!part.empty() && isProtectedName(part))
      return true;
    if (end == std::string::npos)
      break;
    start = end + 1;
  }
  return false;
}

std::string basenameOf(const std::string &path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string parentOf(const std::string &path) {
  const size_t slash = path.find_last_of('/');
  if (slash == std::string::npos || slash == 0)
    return "/";
  return path.substr(0, slash);
}

bool sendAll(int client, const void *data, size_t len) {
  const auto *ptr = static_cast<const char *>(data);
  while (len > 0) {
    const ssize_t written = send(client, ptr, len, 0);
    if (written <= 0)
      return false;
    ptr += written;
    len -= static_cast<size_t>(written);
  }
  return true;
}

bool sendAll(int client, const std::string &data) {
  return sendAll(client, data.data(), data.size());
}

void sendResponse(int client, int status, const std::string &type,
                  const std::string &body,
                  const std::string &extraHeaders = "") {
  const char *reason = "OK";
  if (status == 201)
    reason = "Created";
  if (status == 204)
    reason = "No Content";
  if (status == 207)
    reason = "Multi-Status";
  if (status == 400)
    reason = "Bad Request";
  if (status == 403)
    reason = "Forbidden";
  if (status == 404)
    reason = "Not Found";
  if (status == 409)
    reason = "Conflict";
  if (status == 412)
    reason = "Precondition Failed";
  if (status == 413)
    reason = "Payload Too Large";
  if (status == 500)
    reason = "Internal Server Error";
  if (status == 501)
    reason = "Not Implemented";

  std::ostringstream out;
  out << "HTTP/1.1 " << status << " " << reason << "\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Content-Type: " << type << "\r\n"
      << "Connection: close\r\n"
      << "Access-Control-Allow-Origin: *\r\n"
      << extraHeaders << "\r\n";
  sendAll(client, out.str());
  if (!body.empty())
    sendAll(client, body);
}

std::map<std::string, std::string> parseQuery(const std::string &query) {
  std::map<std::string, std::string> out;
  size_t start = 0;
  while (start <= query.size()) {
    const size_t amp = query.find('&', start);
    std::string item = query.substr(
        start, amp == std::string::npos ? std::string::npos : amp - start);
    const size_t eq = item.find('=');
    if (eq == std::string::npos) {
      out[urlDecode(item)] = "";
    } else {
      out[urlDecode(item.substr(0, eq))] = urlDecode(item.substr(eq + 1));
    }
    if (amp == std::string::npos)
      break;
    start = amp + 1;
  }
  return out;
}

bool parseRequest(int client, Request &req) {
  std::string raw;
  char buffer[8192];
  while (raw.find("\r\n\r\n") == std::string::npos) {
    const ssize_t got = recv(client, buffer, sizeof(buffer), 0);
    if (got <= 0)
      return false;
    raw.append(buffer, static_cast<size_t>(got));
    if (raw.size() > 1024 * 1024)
      return false;
  }

  const size_t headerEnd = raw.find("\r\n\r\n");
  const std::string headers = raw.substr(0, headerEnd);
  req.body = raw.substr(headerEnd + 4);

  std::istringstream stream(headers);
  std::string requestLine;
  if (!std::getline(stream, requestLine))
    return false;
  if (!requestLine.empty() && requestLine.back() == '\r')
    requestLine.pop_back();
  std::istringstream requestLineStream(requestLine);
  requestLineStream >> req.method >> req.target;
  if (req.method.empty() || req.target.empty())
    return false;

  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    const size_t colon = line.find(':');
    if (colon == std::string::npos)
      continue;
    req.headers[lower(line.substr(0, colon))] = trim(line.substr(colon + 1));
  }

  const size_t queryStart = req.target.find('?');
  req.path = normalizePath(req.target.substr(0, queryStart));
  if (queryStart != std::string::npos) {
    req.query = parseQuery(req.target.substr(queryStart + 1));
  }

  size_t contentLength = 0;
  auto contentLengthHeader = req.headers.find("content-length");
  if (contentLengthHeader != req.headers.end()) {
    contentLength = static_cast<size_t>(
        std::strtoull(contentLengthHeader->second.c_str(), nullptr, 10));
  }
  if (contentLength > MAX_BODY_SIZE)
    return false;
  while (req.body.size() < contentLength) {
    const ssize_t got =
        recv(client, buffer,
             std::min(sizeof(buffer), contentLength - req.body.size()), 0);
    if (got <= 0)
      return false;
    req.body.append(buffer, static_cast<size_t>(got));
  }
  if (req.body.size() > contentLength)
    req.body.resize(contentLength);
  return true;
}

std::string queryValue(const Request &req, const std::string &key,
                       const std::string &fallback = "") {
  auto value = req.query.find(key);
  return value == req.query.end() ? fallback : value->second;
}

std::string headerSafeFilename(const std::string &filename) {
  std::string out;
  out.reserve(filename.size());
  for (const char c : filename) {
    if (c == '\r' || c == '\n')
      continue;
    else if (c == '"' || c == '\\')
      out.push_back('_');
    else
      out.push_back(c);
  }
  return out.empty() ? "download" : out;
}

std::string readFormValue(const std::string &body, const std::string &key) {
  const auto values = parseQuery(body);
  auto it = values.find(key);
  return it == values.end() ? "" : it->second;
}

std::string xmlEscape(const std::string &input) {
  std::string out;
  out.reserve(input.size() + 8);
  for (const char c : input) {
    switch (c) {
    case '&':
      out += "&amp;";
      break;
    case '<':
      out += "&lt;";
      break;
    case '>':
      out += "&gt;";
      break;
    case '"':
      out += "&quot;";
      break;
    case '\'':
      out += "&apos;";
      break;
    default:
      out.push_back(c);
      break;
    }
  }
  return out;
}

std::string destinationPath(const Request &req) {
  auto it = req.headers.find("destination");
  if (it == req.headers.end())
    return {};

  std::string value = it->second;
  const size_t scheme = value.find("://");
  if (scheme != std::string::npos) {
    const size_t pathStart = value.find('/', scheme + 3);
    value = pathStart == std::string::npos ? "/" : value.substr(pathStart);
  }
  const size_t queryStart = value.find('?');
  if (queryStart != std::string::npos) {
    value = value.substr(0, queryStart);
  }
  return normalizePath(value);
}

bool shouldOverwriteDestination(const Request &req) {
  auto it = req.headers.find("overwrite");
  return it == req.headers.end() || lower(it->second) != "f";
}

bool copyPath(const std::string &source, const std::string &dest) {
  HalFile input;
  if (!Storage.openFileForRead("WEBSIM", source.c_str(), input))
    return false;
  if (input.isDirectory()) {
    if (!Storage.mkdir(dest.c_str())) {
      input.close();
      return false;
    }
    HalFile child = input.openNextFile();
    while (child) {
      char name[500] = {0};
      child.getName(name, sizeof(name));
      const std::string fileName = name;
      child.close();
      if (!isProtectedName(fileName)) {
        const std::string childSource =
            (source == "/" ? "/" : source + "/") + fileName;
        const std::string childDest =
            (dest == "/" ? "/" : dest + "/") + fileName;
        if (!copyPath(childSource, childDest)) {
          input.close();
          return false;
        }
      }
      child = input.openNextFile();
    }
    input.close();
    return true;
  }

  HalFile output;
  if (!Storage.openFileForWrite("WEBSIM", dest.c_str(), output)) {
    input.close();
    return false;
  }

  std::array<uint8_t, 16384> buffer{};
  bool ok = true;
  while (input.available()) {
    const int got = input.read(buffer.data(), buffer.size());
    if (got <= 0)
      break;
    if (output.write(buffer.data(), static_cast<size_t>(got)) !=
        static_cast<size_t>(got)) {
      ok = false;
      break;
    }
  }
  input.close();
  output.close();
  if (!ok)
    Storage.remove(dest.c_str());
  return ok;
}

void appendPropEntry(std::ostringstream &out, const std::string &path,
                     bool isDirectory, size_t size) {
  std::string href = path.empty() ? "/" : path;
  if (isDirectory && href.back() != '/')
    href += "/";
  out << "<D:response><D:href>" << xmlEscape(href)
      << "</D:href><D:propstat><D:prop>"
      << "<D:resourcetype>";
  if (isDirectory)
    out << "<D:collection/>";
  out << "</D:resourcetype>"
      << "<D:getcontentlength>" << (isDirectory ? 0 : size)
      << "</D:getcontentlength>"
      << "<D:getlastmodified>Thu, 01 Jan 2024 00:00:00 GMT</D:getlastmodified>"
      << "</D:prop><D:status>HTTP/1.1 200 "
         "OK</D:status></D:propstat></D:response>";
}

std::string propfindXml(const std::string &path, int depth) {
  std::ostringstream out;
  out << "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
      << "<D:multistatus xmlns:D=\"DAV:\">";

  HalFile root = Storage.open(path.c_str());
  if (!root) {
    out << "</D:multistatus>";
    return out.str();
  }

  const bool rootIsDirectory = root.isDirectory();
  appendPropEntry(out, path, rootIsDirectory, root.size());

  if (rootIsDirectory && depth > 0) {
    HalFile file = root.openNextFile();
    while (file) {
      char name[500] = {0};
      file.getName(name, sizeof(name));
      const std::string fileName = name;
      if (!isProtectedName(fileName)) {
        const std::string childPath =
            (path == "/" ? "/" : path + "/") + fileName;
        appendPropEntry(out, childPath, file.isDirectory(), file.size());
      }
      file.close();
      file = root.openNextFile();
    }
  }

  root.close();
  out << "</D:multistatus>";
  return out.str();
}

std::string listFilesJson(const std::string &dirPath) {
  FsFile root = Storage.open(dirPath.c_str());
  if (!root || !root.isDirectory())
    return "[]";

  std::ostringstream out;
  out << "[";
  bool first = true;
  FsFile file = root.openNextFile();
  while (file) {
    char name[500] = {0};
    file.getName(name, sizeof(name));
    const std::string fileName = name;
    if (!isProtectedName(fileName)) {
      if (!first)
        out << ",";
      first = false;
      const bool isDirectory = file.isDirectory();
      const bool isEpub =
          fileName.size() >= 5 &&
          lower(fileName.substr(fileName.size() - 5)) == ".epub";
      out << "{\"name\":\"" << jsonEscape(fileName)
          << "\",\"size\":" << (isDirectory ? 0 : file.size())
          << ",\"isDirectory\":" << (isDirectory ? "true" : "false")
          << ",\"isEpub\":" << (isEpub ? "true" : "false") << "}";
    }
    file.close();
    file = root.openNextFile();
  }
  root.close();
  out << "]";
  return out.str();
}

bool writeFile(const std::string &path, const char *data, size_t size) {
  HalFile file;
  if (!Storage.openFileForWrite("WEBSIM", path.c_str(), file))
    return false;
  const size_t written =
      file.write(reinterpret_cast<const uint8_t *>(data), size);
  file.close();
  return written == size;
}

std::string multipartBoundary(const Request &req) {
  auto it = req.headers.find("content-type");
  if (it == req.headers.end())
    return {};
  const std::string marker = "boundary=";
  const size_t pos = it->second.find(marker);
  return pos == std::string::npos
             ? ""
             : "--" + it->second.substr(pos + marker.size());
}

bool parseMultipartFile(const Request &req, std::string &filename,
                        std::string &bytes) {
  const std::string boundary = multipartBoundary(req);
  if (boundary.empty())
    return false;
  size_t partStart = req.body.find(boundary);
  while (partStart != std::string::npos) {
    partStart += boundary.size();
    if (req.body.compare(partStart, 2, "--") == 0)
      break;
    if (req.body.compare(partStart, 2, "\r\n") == 0)
      partStart += 2;
    const size_t headerEnd = req.body.find("\r\n\r\n", partStart);
    if (headerEnd == std::string::npos)
      return false;
    const std::string partHeaders =
        req.body.substr(partStart, headerEnd - partStart);
    const size_t filenamePos = partHeaders.find("filename=\"");
    const size_t contentStart = headerEnd + 4;
    const size_t nextBoundary = req.body.find("\r\n" + boundary, contentStart);
    if (nextBoundary == std::string::npos)
      return false;
    if (filenamePos != std::string::npos) {
      const size_t valueStart = filenamePos + 10;
      const size_t valueEnd = partHeaders.find('"', valueStart);
      if (valueEnd == std::string::npos)
        return false;
      filename =
          basenameOf(partHeaders.substr(valueStart, valueEnd - valueStart));
      bytes = req.body.substr(contentStart, nextBoundary - contentStart);
      return true;
    }
    partStart = req.body.find(boundary, nextBoundary);
  }
  return false;
}

std::string htmlPage() {
  return R"HTML(<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>CrossVi Simulator</title>
<style>
body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;margin:24px;max-width:980px}
button,input{font:inherit;margin:4px}
table{width:100%;border-collapse:collapse;margin-top:16px}
td,th{border-bottom:1px solid #ddd;padding:8px;text-align:left}
.bar{display:flex;flex-wrap:wrap;gap:8px;align-items:center}
.path{font-weight:600}
</style>
</head>
<body>
<h1>CrossVi Simulator</h1>
<div class="bar">
<button onclick="up()">Up</button>
<span class="path" id="path"></span>
<input id="folder" placeholder="Folder name">
<button onclick="mkdir()">Create folder</button>
</div>
<div class="bar">
<input id="file" type="file">
<button onclick="upload()">Upload</button>
</div>
<table>
<thead><tr><th>Name</th><th>Size</th><th>Actions</th></tr></thead>
<tbody id="files"></tbody>
</table>
	<script>
	let path="/";
	const enc=encodeURIComponent;
	function html(s){return String(s).replace(/[&<>"']/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;","\"":"&quot;","'":"&#39;"}[c]))}
	function js(s){return String(s).replace(/\\/g,"\\\\").replace(/'/g,"\\'")}
	function join(a,b){return (a==="/" ? "" : a)+"/"+b}
	async function api(url,opts){const r=await fetch(url,opts); if(!r.ok) throw new Error(await r.text()); return r}
	async function load(){
	  document.getElementById("path").textContent=path;
	  const data=await (await fetch("/api/files?path="+enc(path))).json();
	  document.getElementById("files").innerHTML=data.map(f=>`
	    <tr>
	      <td>${f.isDirectory?`<a href="#" onclick="openDir('${js(f.name)}')">${html(f.name)}/</a>`:html(f.name)}</td>
	      <td>${f.isDirectory?"":f.size}</td>
	      <td>
	        ${f.isDirectory?"":`<a href="/download?path=${enc(join(path,f.name))}">Download</a>`}
	        <button onclick="renameItem('${js(f.name)}')">Rename</button>
	        <button onclick="deleteItem('${js(f.name)}')">Delete</button>
	      </td>
	    </tr>`).join("");
	}
function openDir(name){path=join(path,name); load()}
function up(){if(path!=="/"){path=path.split("/").slice(0,-1).join("/")||"/"; load()}}
async function mkdir(){
  const name=document.getElementById("folder").value;
  await api("/mkdir",{method:"POST",headers:{"Content-Type":"application/x-www-form-urlencoded"},body:"path="+enc(path)+"&name="+enc(name)});
  document.getElementById("folder").value=""; load();
}
async function upload(){
  const file=document.getElementById("file").files[0]; if(!file)return;
  const data=new FormData(); data.append("file",file);
  await api("/upload?path="+enc(path),{method:"POST",body:data});
  document.getElementById("file").value=""; load();
}
async function renameItem(name){
  const next=prompt("New name",name); if(!next||next===name)return;
  await api("/rename",{method:"POST",headers:{"Content-Type":"application/x-www-form-urlencoded"},body:"path="+enc(join(path,name))+"&name="+enc(next)});
  load();
}
async function deleteItem(name){
  if(!confirm("Delete "+name+"?"))return;
  await api("/delete",{method:"POST",headers:{"Content-Type":"application/x-www-form-urlencoded"},body:"path="+enc(join(path,name))});
  load();
}
load().catch(e=>alert(e.message));
</script>
</body>
</html>)HTML";
}

void handleDownload(int client, const std::string &path) {
  if (path.empty() || path == "/" || isProtectedPath(path) ||
      !Storage.exists(path.c_str())) {
    sendResponse(client, 404, "text/plain", "Not found");
    return;
  }
  HalFile file = Storage.open(path.c_str());
  if (!file || file.isDirectory()) {
    sendResponse(client, 400, "text/plain", "Not a file");
    return;
  }
  std::ostringstream headers;
  headers << "HTTP/1.1 200 OK\r\n"
          << "Content-Length: " << file.size() << "\r\n"
          << "Content-Type: application/octet-stream\r\n"
          << "Content-Disposition: attachment; filename=\""
          << headerSafeFilename(basenameOf(path)) << "\"\r\n"
          << "Connection: close\r\n\r\n";
  sendAll(client, headers.str());
  std::array<uint8_t, 16384> buffer{};
  while (file.available()) {
    const int got = file.read(buffer.data(), buffer.size());
    if (got <= 0)
      break;
    if (!sendAll(client, buffer.data(), static_cast<size_t>(got)))
      break;
  }
  file.close();
}

void handleDownloadHead(int client, const std::string &path, HalFile &file) {
  std::ostringstream headers;
  headers << "HTTP/1.1 200 OK\r\n"
          << "Content-Length: " << file.size() << "\r\n"
          << "Content-Type: application/octet-stream\r\n"
          << "Content-Disposition: attachment; filename=\""
          << headerSafeFilename(basenameOf(path)) << "\"\r\n"
          << "Connection: close\r\n"
          << "Access-Control-Allow-Origin: *\r\n\r\n";
  sendAll(client, headers.str());
}

void handleClientRequest(CrossPointWebServer *owner, NativeServerState &state,
                         int client) {
  Request req;
  if (!parseRequest(client, req) || req.path.empty()) {
    sendResponse(client, 400, "text/plain", "Bad request");
    return;
  }

  LOG_DBG("WEB", "[SIM] %s %s", req.method.c_str(), req.target.c_str());

  if (req.method == "OPTIONS") {
    sendResponse(client, 204, "text/plain", "",
                 "DAV: 1\r\nAllow: OPTIONS, GET, HEAD, POST, PUT, DELETE, "
                 "PROPFIND, MKCOL, MOVE, COPY\r\n"
                 "MS-Author-Via: DAV\r\n");
    return;
  }
  if (req.method == "LOCK" || req.method == "UNLOCK") {
    LOG_DBG("WEB",
            "[SIM] WebDAV locks are not enforced by the native simulator");
    sendResponse(client, 501, "text/plain",
                 "[SIM] WebDAV locks are not supported by the native simulator",
                 "DAV: 1\r\nX-CrossVi-Simulator: lock-not-enforced\r\n");
    return;
  }
  if (req.method == "PROPFIND") {
    const std::string path = normalizePath(req.path);
    if (path.empty() || isProtectedPath(path)) {
      sendResponse(client, 403, "text/plain", "Forbidden");
      return;
    }
    if (!Storage.exists(path.c_str()) && path != "/") {
      sendResponse(client, 404, "text/plain", "Not found");
      return;
    }
    int depth = 1;
    auto depthHeader = req.headers.find("depth");
    if (depthHeader != req.headers.end() && depthHeader->second == "0")
      depth = 0;
    sendResponse(client, 207, "application/xml; charset=\"utf-8\"",
                 propfindXml(path, depth), "DAV: 1\r\n");
    return;
  }
  if (req.method == "GET" && (req.path == "/" || req.path == "/files")) {
    sendResponse(client, 200, "text/html; charset=utf-8", htmlPage());
    return;
  }
  if (req.method == "GET" && req.path == "/api/status") {
    const std::string body = std::string("{\"version\":\"") +
                             CROSSPOINT_VERSION +
                             "\",\"ip\":\"127.0.0.1\",\"mode\":\"SIM\"}";
    sendResponse(client, 200, "application/json", body);
    return;
  }
  if (req.method == "GET" && req.path == "/api/files") {
    const std::string path = normalizePath(queryValue(req, "path", "/"));
    if (path.empty() || isProtectedPath(path)) {
      sendResponse(client, 403, "text/plain", "Forbidden");
      return;
    }
    sendResponse(client, 200, "application/json", listFilesJson(path));
    return;
  }
  if ((req.method == "GET" || req.method == "HEAD") &&
      req.path == "/download") {
    const std::string path = normalizePath(queryValue(req, "path"));
    if (path.empty() || path == "/" || isProtectedPath(path) ||
        !Storage.exists(path.c_str())) {
      sendResponse(client, 404, "text/plain", "Not found");
      return;
    }
    HalFile file = Storage.open(path.c_str());
    if (!file || file.isDirectory()) {
      sendResponse(client, 400, "text/plain", "Not a file");
      if (file)
        file.close();
      return;
    }
    if (req.method == "HEAD") {
      handleDownloadHead(client, path, file);
      file.close();
    } else {
      file.close();
      handleDownload(client, path);
    }
    return;
  }
  if (req.method == "POST" && req.path == "/upload") {
    std::string filename;
    std::string bytes;
    if (!parseMultipartFile(req, filename, bytes) ||
        isProtectedName(filename)) {
      sendResponse(client, 400, "text/plain", "Missing file");
      return;
    }
    std::string dir = normalizePath(queryValue(req, "path", "/"));
    if (dir.empty() || isProtectedPath(dir)) {
      sendResponse(client, 403, "text/plain", "Forbidden");
      return;
    }
    if (!Storage.exists(dir.c_str()))
      Storage.mkdir(dir.c_str());
    const std::string path = (dir == "/" ? "/" : dir + "/") + filename;
    {
      std::lock_guard<std::mutex> lock(state.uploadMutex);
      state.uploadStatus.inProgress = true;
      state.uploadStatus.filename = filename;
      state.uploadStatus.total = bytes.size();
      state.uploadStatus.received = 0;
    }
    const bool ok = writeFile(path, bytes.data(), bytes.size());
    {
      std::lock_guard<std::mutex> lock(state.uploadMutex);
      state.uploadStatus.inProgress = false;
      state.uploadStatus.received = ok ? bytes.size() : 0;
      state.uploadStatus.lastCompleteName = ok ? filename : "";
      state.uploadStatus.lastCompleteSize = ok ? bytes.size() : 0;
      state.uploadStatus.lastCompleteAt = ok ? millis() : 0;
    }
    sendResponse(client, ok ? 200 : 500, "text/plain",
                 ok ? "Uploaded" : "Upload failed");
    return;
  }
  if (req.method == "POST" && req.path == "/mkdir") {
    const std::string parent =
        normalizePath(readFormValue(req.body, "path").empty()
                          ? "/"
                          : readFormValue(req.body, "path"));
    const std::string name = basenameOf(readFormValue(req.body, "name"));
    if (parent.empty() || isProtectedPath(parent) || isProtectedName(name)) {
      sendResponse(client, 403, "text/plain", "Forbidden");
      return;
    }
    const std::string path = (parent == "/" ? "/" : parent + "/") + name;
    sendResponse(client, Storage.mkdir(path.c_str()) ? 200 : 500, "text/plain",
                 "Folder created");
    return;
  }
  if (req.method == "POST" && req.path == "/rename") {
    const std::string path = normalizePath(readFormValue(req.body, "path"));
    const std::string name = basenameOf(readFormValue(req.body, "name"));
    if (path.empty() || path == "/" || isProtectedPath(path) ||
        isProtectedName(name)) {
      sendResponse(client, 403, "text/plain", "Forbidden");
      return;
    }
    const std::string dest =
        (parentOf(path) == "/" ? "/" : parentOf(path) + "/") + name;
    sendResponse(client, Storage.rename(path.c_str(), dest.c_str()) ? 200 : 500,
                 "text/plain", "Renamed");
    return;
  }
  if (req.method == "POST" && req.path == "/move") {
    const std::string path = normalizePath(readFormValue(req.body, "path"));
    const std::string destDir = normalizePath(readFormValue(req.body, "dest"));
    if (path.empty() || path == "/" || destDir.empty() ||
        isProtectedPath(path) || isProtectedPath(destDir)) {
      sendResponse(client, 403, "text/plain", "Forbidden");
      return;
    }
    const std::string dest =
        (destDir == "/" ? "/" : destDir + "/") + basenameOf(path);
    sendResponse(client, Storage.rename(path.c_str(), dest.c_str()) ? 200 : 500,
                 "text/plain", "Moved");
    return;
  }
  if ((req.method == "POST" && req.path == "/delete") ||
      req.method == "DELETE") {
    const std::string path =
        req.method == "DELETE" ? req.path
                               : normalizePath(readFormValue(req.body, "path"));
    if (path.empty() || path == "/" || isProtectedPath(path)) {
      sendResponse(client, 403, "text/plain", "Forbidden");
      return;
    }
    HalFile file = Storage.open(path.c_str());
    const bool ok = file && file.isDirectory()
                        ? (file.close(), Storage.rmdir(path.c_str()))
                        : (file.close(), Storage.remove(path.c_str()));
    sendResponse(client, ok ? 200 : 500, "text/plain",
                 ok ? "Deleted" : "Delete failed");
    return;
  }
  if (req.method == "PUT") {
    const std::string path = normalizePath(req.path);
    if (path.empty() || path == "/" || isProtectedPath(path)) {
      sendResponse(client, 403, "text/plain", "Forbidden");
      return;
    }
    sendResponse(client,
                 writeFile(path, req.body.data(), req.body.size()) ? 201 : 500,
                 "text/plain", "Stored");
    return;
  }
  if (req.method == "MKCOL") {
    const std::string path = normalizePath(req.path);
    if (path.empty() || path == "/" || isProtectedPath(path)) {
      sendResponse(client, 403, "text/plain", "Forbidden");
      return;
    }
    sendResponse(client, Storage.mkdir(path.c_str()) ? 201 : 500, "text/plain",
                 "Created");
    return;
  }
  if (req.method == "MOVE" || req.method == "COPY") {
    const std::string source = normalizePath(req.path);
    const std::string dest = destinationPath(req);
    if (source.empty() || source == "/" || dest.empty() || dest == "/" ||
        isProtectedPath(source) || isProtectedPath(dest)) {
      sendResponse(client, 403, "text/plain", "Forbidden");
      return;
    }
    if (!Storage.exists(source.c_str())) {
      sendResponse(client, 404, "text/plain", "Not found");
      return;
    }
    if (Storage.exists(dest.c_str())) {
      if (!shouldOverwriteDestination(req)) {
        sendResponse(client, 412, "text/plain", "Destination exists");
        return;
      }
      HalFile existing = Storage.open(dest.c_str());
      const bool removed =
          existing && existing.isDirectory()
              ? (existing.close(), Storage.rmdir(dest.c_str()))
              : (existing.close(), Storage.remove(dest.c_str()));
      if (!removed) {
        sendResponse(client, 500, "text/plain",
                     "Could not overwrite destination");
        return;
      }
    }

    const bool ok = req.method == "MOVE"
                        ? Storage.rename(source.c_str(), dest.c_str())
                        : copyPath(source, dest);
    sendResponse(client, ok ? 201 : 500, "text/plain", ok ? "Done" : "Failed");
    return;
  }

  (void)owner;
  sendResponse(client, 404, "text/plain", "Not found");
}

void acceptLoop(CrossPointWebServer *owner, NativeServerState *state) {
  while (state->active) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(state->fd, &fds);
    timeval timeout{0, 200000};
    int ready = select(state->fd + 1, &fds, nullptr, nullptr, &timeout);
    if (ready <= 0)
      continue;

    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    int client = accept(state->fd, reinterpret_cast<sockaddr *>(&addr), &len);
    if (client < 0)
      continue;
    timeval readTimeout{5, 0};
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &readTimeout,
               sizeof(readTimeout));
    handleClientRequest(owner, *state, client);
    close(client);
  }
}
} // namespace

CrossPointWebServer::CrossPointWebServer() { port = SIMULATOR_WEB_PORT; }

CrossPointWebServer::~CrossPointWebServer() { stop(); }

void CrossPointWebServer::begin() {
  if (running)
    return;

  auto state = std::make_unique<NativeServerState>();
  state->fd = socket(AF_INET, SOCK_STREAM, 0);
  if (state->fd < 0) {
    LOG_ERR("WEB", "[SIM] socket failed: %s", strerror(errno));
    return;
  }

  int yes = 1;
  setsockopt(state->fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);

  if (bind(state->fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    LOG_ERR("WEB", "[SIM] bind 127.0.0.1:%d failed: %s", port, strerror(errno));
    close(state->fd);
    return;
  }
  if (listen(state->fd, 16) != 0) {
    LOG_ERR("WEB", "[SIM] listen failed: %s", strerror(errno));
    close(state->fd);
    return;
  }

  state->active = true;
  auto *statePtr = state.get();
  {
    std::lock_guard<std::mutex> lock(statesMutex);
    states[this] = std::move(state);
  }
  running = true;
  statePtr->worker = std::thread(acceptLoop, this, statePtr);
  LOG_DBG("WEB", "[SIM] File transfer server running at http://127.0.0.1:%d/",
          port);
}

void CrossPointWebServer::stop() {
  std::unique_ptr<NativeServerState> state;
  {
    std::lock_guard<std::mutex> lock(statesMutex);
    auto it = states.find(this);
    if (it != states.end()) {
      state = std::move(it->second);
      states.erase(it);
    }
  }
  running = false;
  if (!state)
    return;
  state->active = false;
  if (state->fd >= 0) {
    shutdown(state->fd, SHUT_RDWR);
    close(state->fd);
    state->fd = -1;
  }
  if (state->worker.joinable())
    state->worker.join();
}

void CrossPointWebServer::handleClient() {}

CrossPointWebServer::WsUploadStatus
CrossPointWebServer::getWsUploadStatus() const {
  std::lock_guard<std::mutex> statesLock(statesMutex);
  auto it = states.find(this);
  if (it == states.end())
    return {};
  std::lock_guard<std::mutex> uploadLock(it->second->uploadMutex);
  return it->second->uploadStatus;
}

#endif // CROSSPOINT_SIMULATOR_PROJECT_WEBSERVER
