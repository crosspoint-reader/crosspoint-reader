#include "PluginHttp.h"

#include <HalStorage.h>
#include <Logging.h>
#include <SecureHttpClient.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr size_t MAX_TOKEN_FILE_SIZE = 2 * 1024;

// Resolves the caller's reused TLS session (or `tmp`) into a configured,
// connected client, or nullptr if begin() failed.
freeink::SecureHttpClient* openClient(freeink::SecureHttpClient* session, freeink::SecureHttpClient& tmp,
                                      const std::string& url, const pluginhttp::Headers& headers) {
  freeink::SecureHttpClient& http = session ? *session : tmp;
  http.setUserAgent("CrossPoint");
  // Same trust posture as every other SecureNet consumer: no CA bundle ships
  // with the transport, so verification is skipped; traffic stays encrypted.
  http.setInsecure();
  http.setTimeout(30000);
  if (!http.begin(url)) return nullptr;
  for (const auto& h : headers) http.addHeader(h.first, h.second);
  return &http;
}
}  // namespace

namespace pluginhttp {

bool segIsIndex(const std::string& seg) { return !seg.empty() && isdigit(static_cast<unsigned char>(seg[0])); }

void splitPath(const std::string& dotted, std::vector<std::string>& out) {
  out.clear();
  size_t start = 0;
  while (start <= dotted.size()) {
    const size_t dot = dotted.find('.', start);
    if (dot == std::string::npos) {
      if (start < dotted.size()) out.push_back(dotted.substr(start));
      break;
    }
    if (dot > start) out.push_back(dotted.substr(start, dot - start));
    start = dot + 1;
  }
}

JsonVariantConst resolvePath(JsonVariantConst node, const std::string& dotted) {
  if (dotted.empty()) return node;
  std::vector<std::string> segs;
  splitPath(dotted, segs);
  for (const auto& seg : segs) {
    if (node.isNull()) break;
    if (segIsIndex(seg)) {
      node = node[atoi(seg.c_str())];
    } else {
      node = node[seg.c_str()];
    }
  }
  return node;
}

std::string variantToString(JsonVariantConst v) {
  if (v.isNull()) return "";
  if (v.is<const char*>()) return v.as<const char*>();
  char buf[24];
  if (v.is<long long>() || v.is<int>()) {
    snprintf(buf, sizeof(buf), "%lld", v.as<long long>());
    return buf;
  }
  return "";
}

void substituteAll(std::string& s, const char* key, const std::string& value) {
  const size_t keyLen = strlen(key);
  size_t pos = 0;
  while ((pos = s.find(key, pos)) != std::string::npos) {
    s.replace(pos, keyLen, value);
    pos += value.size();
  }
}

void readRequest(JsonVariantConst node, const char* defaultMethod, RequestSpec& out) {
  out.url = node["url"] | "";
  out.method = node["method"] | defaultMethod;
  out.body = node["body"] | "";
  readHeaders(node["headers"], out.headers);
}

void readHeaders(JsonVariantConst node, Headers& out) {
  out.clear();
  for (JsonPairConst kv : node.as<JsonObjectConst>()) {
    out.emplace_back(kv.key().c_str(), kv.value().as<const char*>() ? kv.value().as<const char*>() : "");
  }
}

std::string urlEncodeQuery(const std::string& s) {
  std::string out;
  out.reserve(s.size() * 3);
  for (const unsigned char c : s) {
    if (isalnum(c) || strchr("-_.~", c)) {
      out += static_cast<char>(c);
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", c);
      out += buf;
    }
  }
  return out;
}

bool loadTokenFromFile(const std::string& file, const std::string& path, std::string& out) {
  out.clear();
  if (file.empty()) return true;  // token-less plugin
  std::string raw;
  if (!Storage.readFileToString("PHTP", file, MAX_TOKEN_FILE_SIZE, raw)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, raw) != DeserializationError::Ok) return false;
  out = variantToString(resolvePath(doc.as<JsonVariantConst>(), path));
  return !out.empty();
}

bool saveTokenToFile(const std::string& file, const std::string& path, const std::string& value) {
  if (file.empty()) return false;
  JsonDocument doc;
  // Build the nesting the read path expects (numeric segments unsupported).
  std::vector<std::string> segs;
  splitPath(path, segs);
  if (segs.empty()) return false;
  JsonVariant node = doc.to<JsonObject>();
  for (size_t i = 0; i + 1 < segs.size(); i++) node = node[segs[i]].to<JsonObject>();
  node[segs.back()] = value;
  std::string out;
  serializeJson(doc, out);
  HalFile f;
  if (!Storage.openFileForWrite("PHTP", file, f)) return false;
  const size_t written = f.write(out.data(), out.size());
  f.flush();
  if (written != out.size()) {
    // A short write (SD full, I/O fault) must not persist a truncated token:
    // the writer would report success, then every later load fails.
    LOG_ERR("PHTP", "short token write (%u of %u)", static_cast<unsigned>(written), static_cast<unsigned>(out.size()));
    if (f.isOpen()) f.close();
    Storage.remove(file.c_str());
    return false;
  }
  return true;
}

void loadConfigFile(const std::string& file, Headers& out) {
  out.clear();
  if (file.empty()) return;
  std::string raw;
  if (!Storage.readFileToString("PHTP", file, MAX_TOKEN_FILE_SIZE, raw)) return;
  JsonDocument doc;
  if (deserializeJson(doc, raw) != DeserializationError::Ok) return;
  for (JsonPairConst kv : doc.as<JsonObjectConst>()) {
    out.emplace_back(kv.key().c_str(), variantToString(kv.value()));
  }
}

int request(freeink::SecureHttpClient* session, const std::string& url, const std::string& method,
            const std::string& body, const Headers& headers, std::string& out, const size_t maxResponse) {
  freeink::SecureHttpClient tmp;
  freeink::SecureHttpClient* httpPtr = openClient(session, tmp, url, headers);
  if (!httpPtr) return -1;
  freeink::SecureHttpClient& http = *httpPtr;

  out.clear();
  bool overflow = false;
  const int status = http.sendRequest(method.c_str(), reinterpret_cast<const uint8_t*>(body.data()), body.size(),
                                      [&](const uint8_t* data, size_t len) {
                                        if (out.size() + len > maxResponse) {
                                          overflow = true;
                                          return false;
                                        }
                                        // Grow the buffer only as needed, and only when a nothrow probe proves
                                        // the larger block is available. A bare string reallocation would abort
                                        // the device on low heap when a response is large (e.g. an API that
                                        // inlines article HTML); here we just stop and fail the request.
                                        if (out.size() + len > out.capacity()) {
                                          size_t want = out.capacity() ? out.capacity() * 2 : 2048;
                                          if (want < out.size() + len) want = out.size() + len;
                                          if (want > maxResponse) want = maxResponse;
                                          void* probe = malloc(want + 256);
                                          if (!probe) {
                                            overflow = true;
                                            return false;
                                          }
                                          free(probe);
                                          out.reserve(want);
                                        }
                                        out.append(reinterpret_cast<const char*>(data), len);
                                        return true;
                                      });
  // Error statuses still return their body: OAuth device-code polling carries
  // its state ("authorization_pending") in 4xx response bodies.
  if (overflow || status < 0 || !http.responseComplete()) {
    LOG_ERR("PHTP", "API request failed: status=%d overflow=%d complete=%d %s", status, overflow,
            http.responseComplete(), url.c_str());
    return -1;
  }
  return status;
}

int requestToFile(freeink::SecureHttpClient* session, const std::string& url, const std::string& method,
                  const std::string& body, const Headers& headers, const char* destPath, const size_t maxResponse) {
  freeink::SecureHttpClient tmp;
  freeink::SecureHttpClient* httpPtr = openClient(session, tmp, url, headers);
  if (!httpPtr) return -1;
  freeink::SecureHttpClient& http = *httpPtr;

  int status = -1;
  bool writeOk = true;
  size_t written = 0;
  {
    HalFile file;
    if (!Storage.openFileForWrite("PHTP", destPath, file)) return -1;
    status = http.sendRequest(method.c_str(), reinterpret_cast<const uint8_t*>(body.data()), body.size(),
                              [&](const uint8_t* data, size_t len) {
                                if (written + len > maxResponse) {
                                  writeOk = false;
                                  return false;
                                }
                                if (file.write(data, len) != len) {
                                  writeOk = false;
                                  return false;
                                }
                                written += len;
                                return true;
                              });
    file.flush();
    // file closed at scope exit, before any Storage.remove of destPath
  }
  if (!writeOk || status < 0 || !http.responseComplete()) {
    LOG_ERR("PHTP", "API request (to file) failed: status=%d writeOk=%d complete=%d %s", status, writeOk,
            http.responseComplete(), url.c_str());
    Storage.remove(destPath);
    return -1;
  }
  return status;
}

bool mintPasswordToken(freeink::SecureHttpClient* session, const std::string& url, const std::string& method,
                       const std::string& body, const Headers& headers, const std::string& tokenPath,
                       std::string& outToken) {
  std::string response;
  constexpr size_t MAX_AUTH_RESPONSE = 8 * 1024;
  const int status = request(session, url, method, body, headers, response, MAX_AUTH_RESPONSE);
  if (status < 200 || status >= 300) return false;
  JsonDocument doc;
  if (deserializeJson(doc, response) != DeserializationError::Ok) return false;
  outToken = variantToString(resolvePath(doc.as<JsonVariantConst>(), tokenPath));
  return !outToken.empty();
}

}  // namespace pluginhttp
