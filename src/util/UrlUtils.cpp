#include "UrlUtils.h"

#include <cstdio>

namespace UrlUtils {
namespace {
bool isHexDigit(const char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'); }

bool shouldEncode(const unsigned char c) {
  if (c <= 0x20 || c >= 0x7f) return true;
  switch (c) {
    case '"':
    case '<':
    case '>':
    case '\\':
    case '^':
    case '`':
    case '{':
    case '|':
    case '}':
      return true;
    default:
      return false;
  }
}
}  // namespace

std::string ensureProtocol(const std::string& url) {
  if (url.find("://") == std::string::npos) {
    return "http://" + url;
  }
  return url;
}

std::string extractHost(const std::string& url) {
  const size_t protocolEnd = url.find("://");
  if (protocolEnd == std::string::npos) {
    // No protocol, find first slash
    const size_t firstSlash = url.find_first_of("/?#");
    return firstSlash == std::string::npos ? url : url.substr(0, firstSlash);
  }
  // Find the first slash after the protocol
  const size_t hostStart = protocolEnd + 3;
  const size_t pathStart = url.find_first_of("/?#", hostStart);
  return pathStart == std::string::npos ? url : url.substr(0, pathStart);
}

std::string encodeUnsafeUrlChars(const std::string& url) {
  std::string out;
  out.reserve(url.size());
  for (size_t i = 0; i < url.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(url[i]);
    if (c == '%' && i + 2 < url.size() && isHexDigit(url[i + 1]) && isHexDigit(url[i + 2])) {
      out += url[i];
      out += url[i + 1];
      out += url[i + 2];
      i += 2;
    } else if (c == '%' || shouldEncode(c)) {
      char encoded[4];
      snprintf(encoded, sizeof(encoded), "%%%02X", c);
      out += encoded;
    } else {
      out += static_cast<char>(c);
    }
  }
  return out;
}

std::string buildUrl(const std::string& serverUrl, const std::string& path) {
  if (path.find("://") != std::string::npos) return encodeUnsafeUrlChars(path);
  std::string base = ensureProtocol(serverUrl);
  const size_t fragment = base.find('#');
  if (fragment != std::string::npos) base.resize(fragment);
  if (path.empty()) return encodeUnsafeUrlChars(base);
  if (path[0] == '#') return encodeUnsafeUrlChars(base + path);
  const size_t query = base.find('?');
  if (query != std::string::npos) base.resize(query);
  if (path[0] == '?') return encodeUnsafeUrlChars(base + path);
  if (path.starts_with("//")) return encodeUnsafeUrlChars(base.substr(0, base.find(':') + 1) + path);
  const std::string origin = extractHost(base);
  std::string resolved;
  if (path[0] == '/')
    resolved = path;
  else {
    const size_t slash = base.rfind('/');
    const std::string parent = slash < origin.size() ? "/" : base.substr(origin.size(), slash - origin.size() + 1);
    resolved = parent + path;
  }
  const size_t suffix = resolved.find_first_of("?#");
  const std::string tail = suffix == std::string::npos ? "" : resolved.substr(suffix);
  if (suffix != std::string::npos) resolved.resize(suffix);
  std::string normalized;
  while (!resolved.empty()) {
    if (resolved.starts_with("../"))
      resolved.erase(0, 3);
    else if (resolved.starts_with("./"))
      resolved.erase(0, 2);
    else if (resolved.starts_with("/./"))
      resolved.erase(0, 2);
    else if (resolved == "/.")
      resolved = "/";
    else if (resolved.starts_with("/../") || resolved == "/..") {
      resolved.replace(0, 3, "");
      if (resolved.empty()) resolved = "/";
      const size_t slash = normalized.rfind('/');
      normalized.resize(slash == std::string::npos ? 0 : slash);
    } else if (resolved == "." || resolved == "..")
      resolved.clear();
    else {
      const size_t slash = resolved.find('/', resolved[0] == '/' ? 1 : 0);
      const size_t count = slash == std::string::npos ? resolved.size() : slash;
      normalized.append(resolved, 0, count);
      resolved.erase(0, count);
    }
  }
  return encodeUnsafeUrlChars(origin + normalized + tail);
}

}  // namespace UrlUtils
