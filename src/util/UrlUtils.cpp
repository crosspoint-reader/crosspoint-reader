#include "UrlUtils.h"

#include <sstream>
#include <iomanip>

namespace UrlUtils {

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
    const size_t firstSlash = url.find('/');
    return firstSlash == std::string::npos ? url : url.substr(0, firstSlash);
  }
  // Find the first slash after the protocol
  const size_t hostStart = protocolEnd + 3;
  const size_t pathStart = url.find('/', hostStart);
  return pathStart == std::string::npos ? url : url.substr(0, pathStart);
}

std::string buildUrl(const std::string& serverUrl, const std::string& path) {
  const std::string urlWithProtocol = ensureProtocol(serverUrl);
  if (path.empty()) {
    return urlWithProtocol;
  }
  if (path[0] == '/') {
    // Absolute path - use just the host
    return extractHost(urlWithProtocol) + path;
  }
  // Relative path - append to server URL
  if (urlWithProtocol.back() == '/') {
    return urlWithProtocol + path;
  }
  return urlWithProtocol + "/" + path;
}

std::string urlEncode(const std::string& value) {
  std::ostringstream escaped;
  escaped.fill('0');
  escaped << std::hex;

  for (char c : value) {
    // Keep alphanumeric and other safe characters intact
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      escaped << c;
    } else {
      // Encode special characters
      escaped << std::uppercase;
      escaped << '%' << std::setw(2) << int((unsigned char)c);
      escaped << std::nouppercase;
    }
  }

  return escaped.str();
}

std::string buildUrlWithAuth(const std::string& serverUrl, const std::string& path,
                              const std::string& username, const std::string& password) {
  // If no credentials, use regular buildUrl
  if (username.empty() && password.empty()) {
    return buildUrl(serverUrl, path);
  }

  std::string urlWithProtocol = ensureProtocol(serverUrl);
  
  // Find protocol end
  const size_t protocolEnd = urlWithProtocol.find("://");
  if (protocolEnd == std::string::npos) {
    return buildUrl(serverUrl, path);  // Fallback if no protocol
  }

  // Extract protocol and host parts
  std::string protocol = urlWithProtocol.substr(0, protocolEnd + 3);  // Include ://
  std::string hostAndPath = urlWithProtocol.substr(protocolEnd + 3);

  // Check if auth already exists in URL
  const size_t atPos = hostAndPath.find('@');
  if (atPos != std::string::npos) {
    // Auth already in URL, remove it
    hostAndPath = hostAndPath.substr(atPos + 1);
  }

  // Build auth string with URL encoding
  std::string auth;
  if (!username.empty() || !password.empty()) {
    auth = urlEncode(username) + ":" + urlEncode(password) + "@";
  }

  // Reconstruct URL with auth
  std::string authenticatedUrl = protocol + auth + hostAndPath;

  // Now apply path logic
  if (path.empty()) {
    return authenticatedUrl;
  }
  if (path[0] == '/') {
    // Absolute path - extract just protocol + auth + host
    const size_t firstSlash = hostAndPath.find('/');
    std::string hostOnly = (firstSlash == std::string::npos) ? hostAndPath : hostAndPath.substr(0, firstSlash);
    return protocol + auth + hostOnly + path;
  }
  // Relative path
  if (authenticatedUrl.back() == '/') {
    return authenticatedUrl + path;
  }
  return authenticatedUrl + "/" + path;
}

}  // namespace UrlUtils
