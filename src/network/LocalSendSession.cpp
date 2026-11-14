#include "LocalSendSession.h"
#if (defined(CROSSPOINT_LOCALSEND) && CROSSPOINT_LOCALSEND) || defined(CROSSPOINT_LOCALSEND_HOST_TEST)
#include <ArduinoJson.h>

#include <cctype>
#include <cstring>

#include "LocalSendJson.h"

namespace localsend {
namespace {
bool same(const char* a, const char* b) {
  while (*a && *b)
    if (std::tolower(static_cast<unsigned char>(*a++)) != std::tolower(static_cast<unsigned char>(*b++))) return false;
  return *a == *b;
}
int hex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  c = char(std::tolower(static_cast<unsigned char>(c)));
  return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}
bool decode(const char* in, char* out, size_t capacity) {
  size_t n = 0;
  while (*in) {
    unsigned char c = *in++;
    if (c == '%') {
      if (!in[0] || !in[1] || hex(in[0]) < 0 || hex(in[1]) < 0) return false;
      c = unsigned(hex(in[0]) * 16 + hex(in[1]));
      in += 2;
    } else if (c == '+')
      c = ' ';
    if (c < 32 || c == 127 || n + 1 >= capacity) return false;
    out[n++] = char(c);
  }
  out[n] = 0;
  return true;
}
}  // namespace
int parseRequest(char* header, Request& r) {
  r = {};
  if (!header || std::strlen(header) > 2048) return 431;
  const size_t headerLength = std::strlen(header);
  if (headerLength < 4 || std::memcmp(header + headerLength - 4, "\r\n\r\n", 4)) return 400;
  char* lineEnd = std::strstr(header, "\r\n");
  if (!lineEnd) return 400;
  *lineEnd = 0;
  char* target = std::strchr(header, ' ');
  if (!target) return 400;
  *target++ = 0;
  char* version = std::strchr(target, ' ');
  if (!version || size_t(version - target) > 512) return 400;
  *version++ = 0;
  if (std::strcmp(version, "HTTP/1.1") && std::strcmp(version, "HTTP/1.0")) return 400;
  char* query = std::strchr(target, '?');
  if (query) *query++ = 0;
  if (!std::strcmp(target, "/api/localsend/v2/info"))
    r.route = Route::Info;
  else if (!std::strcmp(target, "/api/localsend/v2/register"))
    r.route = Route::Register;
  else if (!std::strcmp(target, "/api/localsend/v2/prepare-upload"))
    r.route = Route::Prepare;
  else if (!std::strcmp(target, "/api/localsend/v2/upload"))
    r.route = Route::Upload;
  else if (!std::strcmp(target, "/api/localsend/v2/cancel"))
    r.route = Route::Cancel;
  else
    return 404;
  if (std::strcmp(header, r.route == Route::Info ? "GET" : "POST")) return 405;
  unsigned seen = 0;
  while (query && *query) {
    char* next = std::strchr(query, '&');
    if (next) *next++ = 0;
    char* value = std::strchr(query, '=');
    if (!value) return 400;
    *value++ = 0;
    unsigned bit = 0;
    char* output = nullptr;
    size_t capacity = 0;
    if (!std::strcmp(query, "sessionId")) {
      bit = 1;
      output = r.sessionId;
      capacity = sizeof(r.sessionId);
    } else if (!std::strcmp(query, "fileId")) {
      bit = 2;
      output = r.fileId;
      capacity = sizeof(r.fileId);
    } else if (!std::strcmp(query, "token")) {
      bit = 4;
      output = r.token;
      capacity = sizeof(r.token);
    }
    if (bit && ((seen & bit) || !decode(value, output, capacity))) return 400;
    seen |= bit;
    query = next;
  }
  bool hasLength = false;
  char* line = lineEnd + 2;
  while (*line) {
    char* end = std::strstr(line, "\r\n");
    if (!end) return 400;
    *end = 0;
    if (!*line) break;
    char* value = std::strchr(line, ':');
    if (!value) return 400;
    *value++ = 0;
    while (*value == ' ' || *value == '\t') ++value;
    char* tail = value + std::strlen(value);
    while (tail > value && (tail[-1] == ' ' || tail[-1] == '\t')) *--tail = 0;
    if (same(line, "Content-Length")) {
      if (hasLength || !parseLength(value, r.length)) return 400;
      hasLength = true;
    }
    if (same(line, "Transfer-Encoding")) {
      if (r.chunked || !same(value, "chunked")) return 400;
      r.chunked = true;
    }
    if (same(line, "Expect")) {
      if (!same(value, "100-continue")) return 417;
      r.expectContinue = true;
    }
    line = end + 2;
  }
  if (r.chunked && (hasLength || r.route != Route::Upload)) return 400;
  if (r.route != Route::Info && r.route != Route::Cancel && !hasLength && !r.chunked) return 411;
  if (r.route == Route::Info && r.length) return 400;
  if (r.route == Route::Upload && seen != 7) return 400;
  if ((r.route == Route::Prepare || r.route == Route::Register) && (!r.length || r.length > MAX_METADATA)) return 413;
  if (r.route == Route::Cancel && (r.length > MAX_METADATA || !(seen & 1))) return 400;
  return 200;
}
bool safeDestinationFolder(const char* path) {
  if (!path) return false;
  const size_t length = std::strlen(path);
  if (!length || length >= 96) return false;
  // These exceptions are intentional device-visible destination presets.
  if (!std::strcmp(path, "/") || !std::strcmp(path, "/LocalSend") || !std::strcmp(path, "/.sleep") ||
      !std::strcmp(path, "/.sleep-overlay"))
    return true;
  if (length < 6 || std::strncmp(path, "/Books", 6) || (path[6] && path[6] != '/')) return false;
  for (size_t i = 6; i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(path[i]);
    if (c < 32 || c == 127 || std::strchr("\\:*?\"<>|%", c)) return false;
    if (c == '/' && (i + 1 == length || path[i + 1] == '/' || path[i + 1] == '.')) return false;
  }
  return true;
}
bool safeReceiveName(const char* name) {
  if (!name) return false;
  const size_t length = std::strlen(name);
  if (length < 5 || length > 128 || name[0] == '.' || name[0] == ' ') return false;
  for (const auto* p = reinterpret_cast<const unsigned char*>(name); *p; ++p)
    if (*p < 32 || *p == 127 || std::strchr("/\\:*?\"<>|%", *p)) return false;
  const char* suffix = std::strrchr(name, '.');
  if (!suffix) return false;
  const char* allowed[] = {".epub", ".png", ".jpg", ".jpeg", ".bmp"};
  bool supported = false;
  for (const char* ext : allowed) {
    if (same(suffix, ext)) {
      supported = true;
      break;
    }
  }
  if (!supported) return false;
  char stem[5]{};
  size_t n = 0;
  while (n < 4 && name[n] && name[n] != '.') {
    stem[n] = char(std::toupper(static_cast<unsigned char>(name[n])));
    ++n;
  }
  if (!std::strcmp(stem, "CON") || !std::strcmp(stem, "PRN") || !std::strcmp(stem, "AUX") ||
      !std::strcmp(stem, "NUL") ||
      (n == 4 && (!std::strncmp(stem, "COM", 3) || !std::strncmp(stem, "LPT", 3)) && stem[3] >= '1' && stem[3] <= '9'))
    return false;
  return true;
}
bool validFileSignature(const char* name, const uint8_t* prefix, size_t length) {
  if (!safeReceiveName(name) || !prefix) return false;
  const char* suffix = std::strrchr(name, '.');
  if (same(suffix, ".epub")) return length >= 4 && !std::memcmp(prefix, "PK\003\004", 4);
  if (same(suffix, ".png")) return length >= 8 && !std::memcmp(prefix, "\211PNG\r\n\032\n", 8);
  if (same(suffix, ".jpg") || same(suffix, ".jpeg"))
    return length >= 3 && prefix[0] == 0xff && prefix[1] == 0xd8 && prefix[2] == 0xff;
  return length >= 2 && prefix[0] == 'B' && prefix[1] == 'M';
}
bool parseChunkSize(const char* text, uint32_t& value) {
  if (!text || !*text) return false;
  uint32_t result = 0;
  size_t digits = 0;
  while (*text && *text != ';') {
    const int digit = hex(*text++);
    if (digit < 0 || ++digits > 8 || result > (MAX_FILE_BYTES - uint32_t(digit)) / 16) return false;
    result = result * 16 + uint32_t(digit);
  }
  if (!digits) return false;
  while (*text) {
    const unsigned char c = *text++;
    if (c < 32 || c == 127) return false;
  }
  value = result;
  return true;
}
bool parseLength(const char* text, uint32_t& value) {
  if (!text || !*text) return false;
  uint32_t result = 0;
  for (const char* p = text; *p; ++p) {
    if (*p < '0' || *p > '9') return false;
    const uint32_t digit = uint32_t(*p - '0');
    if (result > (MAX_FILE_BYTES - digit) / 10) return false;
    result = result * 10 + digit;
  }
  value = result;
  return true;
}
bool Session::expired(uint32_t now) const { return state != State::Idle && now - lastActivity >= SESSION_TIMEOUT_MS; }
void Session::reset() {
  // Clear per-file slots individually; avoid a multi-KiB aggregate temporary
  // on the ESP32-C3 main task stack when a 16-file batch ends.
  for (auto& file : files) file = {};
  state = State::Idle;
  fileCount = uploadedCount = 0;
  sessionId[0] = 0;
  peer = lastActivity = 0;
}
int Session::prepare(const char* json, size_t length, uint32_t address, uint32_t now, const char* id,
                     const char* secret, const char** rejectionReason) {
  if (rejectionReason) *rejectionReason = nullptr;
  const auto reject = [rejectionReason](int code, const char* reason) {
    if (rejectionReason) *rejectionReason = reason;
    return code;
  };
  if (state != State::Idle) return reject(409, "another transfer is active");
  if (!json || !length || length > MAX_METADATA) return reject(400, "metadata missing or over 8192 bytes");
  if (!id || !secret || std::strlen(id) != 32 || std::strlen(secret) != 32)
    return reject(500, "could not initialize transfer credentials");
  localsend::BoundedJsonAllocator allocator;
  JsonDocument doc(&allocator);
  if (deserializeJson(doc, json, length, DeserializationOption::NestingLimit(8)))
    return reject(400, "metadata JSON invalid or memory limit exceeded");
  const auto entries = doc["files"].as<JsonObjectConst>();
  if (entries.size() == 0 || entries.size() > MAX_OFFER_FILES)
    return reject(400, "offer must contain between 1 and 16 files");
  if (!doc["info"].is<JsonObjectConst>()) return reject(400, "sender info missing or invalid");
  uint64_t total = 0;
  fileCount = uploadedCount = 0;
  for (JsonPairConst entry : entries) {
    const auto offer = entry.value().as<JsonObjectConst>();
    const JsonString name = offer["fileName"].as<JsonString>();
    const JsonString key = entry.key();
    if (!name || name.size() != std::strlen(name.c_str())) return reject(400, "filename missing or contains NUL");
    if (name.size() > 128) return reject(400, "filename exceeds 128-byte limit");
    if (std::strchr(name.c_str(), '/') || std::strchr(name.c_str(), '\\'))
      return reject(400, "folder uploads are not yet supported");
    if (!safeReceiveName(name.c_str())) return reject(400, "filename unsafe or extension unsupported");
    if (!key || !key.size() || key.size() > 128 || key.size() != std::strlen(key.c_str()))
      return reject(400, "file ID missing, oversized or invalid");
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(key.c_str()); *p; ++p)
      if (*p < 32 || *p == 127) return reject(400, "file ID contains control characters");
    if (!offer["size"].is<uint32_t>()) return reject(400, "file size missing, invalid or larger than uint32");
    const uint32_t size = offer["size"].as<uint32_t>();
    if (size < 4 || size > MAX_FILE_BYTES) return reject(400, "each file must be 4 bytes to 1 GiB");
    if (size > MAX_TOTAL_BYTES - total) return reject(400, "offer exceeds total supported size");
    for (uint8_t i = 0; i < fileCount; ++i)
      if (same(files[i].fileName, name.c_str())) return reject(400, "offer contains duplicate destination filenames");
    auto& file = files[fileCount];
    file = {};
    std::memcpy(file.fileName, name.c_str(), name.size() + 1);
    std::memcpy(file.fileId, key.c_str(), key.size() + 1);
    std::memcpy(file.token, secret, 33);
    const auto hash = offer["sha256"];
    if (!hash.isNull()) {
      const JsonString h = hash.as<JsonString>();
      if (!h || h.size() != 64 || std::strlen(h.c_str()) != 64)
        return reject(400, "SHA-256 must be a 64-character hex string");
      for (size_t i = 0; i < 64; ++i) {
        const unsigned char c = h.c_str()[i];
        if (!std::isxdigit(c)) return reject(400, "SHA-256 contains non-hex characters");
        file.sha256[i] = char(std::tolower(c));
      }
    }
    file.expected = size;
    total += size;
    ++fileCount;
  }
  std::memcpy(sessionId, id, 33);
  peer = address;
  lastActivity = now;
  state = State::Prepared;
  return 200;
}
bool Session::cancellable(const char* id, uint32_t address, uint32_t now) const {
  return state != State::Idle && !expired(now) && peer == address && id && !std::strcmp(id, sessionId);
}
OfferedFile* Session::authorizedFile(const char* id, const char* file, const char* secret, uint32_t address,
                                     uint32_t now) {
  if (state != State::Prepared || !cancellable(id, address, now) || !file || !secret) return nullptr;
  for (uint8_t i = 0; i < fileCount; ++i) {
    auto& candidate = files[i];
    if (!candidate.uploaded && !std::strcmp(file, candidate.fileId) && !std::strcmp(secret, candidate.token))
      return &candidate;
  }
  return nullptr;
}
bool Session::authorized(const char* id, const char* file, const char* secret, uint32_t address, uint32_t now) const {
  if (state != State::Prepared || !cancellable(id, address, now) || !file || !secret) return false;
  for (uint8_t i = 0; i < fileCount; ++i) {
    const auto& candidate = files[i];
    if (!candidate.uploaded && !std::strcmp(file, candidate.fileId) && !std::strcmp(secret, candidate.token))
      return true;
  }
  return false;
}
bool Session::consume(OfferedFile& file, size_t length, uint32_t now) {
  if (state != State::Receiving || expired(now) || file.received > file.expected ||
      length > file.expected - file.received) {
    state = State::Failed;
    return false;
  }
  file.received += uint32_t(length);
  lastActivity = now;
  return true;
}
bool Session::complete(const OfferedFile& file) const {
  return state == State::Receiving && file.received == file.expected;
}
bool Session::finishFile(OfferedFile& file, uint32_t now) {
  if (!complete(file) || file.uploaded) return false;
  file.uploaded = true;
  ++uploadedCount;
  lastActivity = now;
  state = State::Prepared;
  return uploadedCount == fileCount;
}
}  // namespace localsend
#endif
