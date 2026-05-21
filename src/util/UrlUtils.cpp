#include "UrlUtils.h"

#include <algorithm>
#include <string_view>

namespace UrlUtils {

namespace {
void popLastSegment(std::string& path) {
  if (path.empty()) {
    return;
  }

  size_t end = path.size();
  if (end > 1 && path.back() == '/') {
    end--;
  }

  while (end > 0 && path[end - 1] != '/') {
    end--;
  }

  path.resize(end);
}

std::string normalizePath(const std::string_view path) {
  if (path.empty()) {
    return "/";
  }

  std::string normalized;
  normalized.reserve(path.size());

  size_t inputPos = 0;
  while (inputPos < path.size()) {
    if (path.compare(inputPos, 3, "../") == 0) {
      inputPos += 3;
      continue;
    }
    if (path.compare(inputPos, 2, "./") == 0) {
      inputPos += 2;
      continue;
    }
    if (path.compare(inputPos, 3, "/./") == 0) {
      inputPos += 2;
      continue;
    }
    if (path.compare(inputPos, 2, "/.") == 0 && inputPos + 2 == path.size()) {
      if (normalized.empty()) {
        normalized.push_back('/');
      } else if (normalized.back() != '/') {
        normalized.push_back('/');
      }
      break;
    }
    if (path.compare(inputPos, 4, "/../") == 0) {
      inputPos += 4;
      popLastSegment(normalized);
      if (normalized.empty() || normalized.back() != '/') {
        normalized.push_back('/');
      }
      continue;
    }
    if (path.compare(inputPos, 3, "/..") == 0 && inputPos + 3 == path.size()) {
      popLastSegment(normalized);
      if (normalized.empty() || normalized.back() != '/') {
        normalized.push_back('/');
      }
      break;
    }
    if (path.compare(inputPos, 1, ".") == 0 && inputPos + 1 == path.size()) {
      break;
    }
    if (path.compare(inputPos, 2, "..") == 0 && inputPos + 2 == path.size()) {
      break;
    }

    const size_t nextSlash = path[inputPos] == '/' ? path.find('/', inputPos + 1) : path.find('/', inputPos);
    if (nextSlash == std::string::npos) {
      normalized.append(path, inputPos, path.size() - inputPos);
      break;
    }

    normalized.append(path, inputPos, nextSlash - inputPos);
    inputPos = nextSlash;
  }

  if (path.front() == '/' && normalized.empty()) {
    return "/";
  }

  if (path.front() == '/' && normalized.size() == 1) {
    return normalized;
  }

  if (path.front() != '/' && normalized.empty()) {
    return ".";
  }

  return normalized;
}

std::string_view stripQueryAndFragment(const std::string_view urlPath) {
  const size_t suffixStart = urlPath.find_first_of("?#");
  return suffixStart == std::string::npos ? urlPath : urlPath.substr(0, suffixStart);
}

std::string_view stripFragment(const std::string_view urlPath) {
  const size_t fragmentStart = urlPath.find('#');
  return fragmentStart == std::string::npos ? urlPath : urlPath.substr(0, fragmentStart);
}

std::string_view extractScheme(const std::string_view url) {
  const size_t protocolEnd = url.find("://");
  return protocolEnd == std::string::npos ? "" : url.substr(0, protocolEnd);
}

struct UrlSuffixParts {
  std::string_view path;
  std::string_view query;
  std::string_view fragment;
};

UrlSuffixParts splitReference(const std::string_view ref) {
  UrlSuffixParts parts;
  const size_t fragmentStart = ref.find('#');
  const size_t queryStart = ref.find('?');
  const size_t pathEnd = std::min(queryStart == std::string::npos ? ref.size() : queryStart,
                                  fragmentStart == std::string::npos ? ref.size() : fragmentStart);

  parts.path = ref.substr(0, pathEnd);
  if (queryStart != std::string::npos && (fragmentStart == std::string::npos || queryStart < fragmentStart)) {
    const size_t queryEnd = fragmentStart == std::string::npos ? ref.size() : fragmentStart;
    parts.query = ref.substr(queryStart, queryEnd - queryStart);
  }
  if (fragmentStart != std::string::npos) {
    parts.fragment = ref.substr(fragmentStart);
  }

  return parts;
}

std::string_view extractHostView(const std::string_view url) {
  const size_t protocolEnd = url.find("://");
  if (protocolEnd == std::string::npos) {
    const size_t authorityEnd = url.find_first_of("/?#");
    return authorityEnd == std::string::npos ? url : url.substr(0, authorityEnd);
  }

  const size_t authorityStart = protocolEnd + 3;
  const size_t authorityEnd = url.find_first_of("/?#", authorityStart);
  return authorityEnd == std::string::npos ? url : url.substr(0, authorityEnd);
}

void appendView(std::string& out, const std::string_view view) { out.append(view.data(), view.size()); }

std::string buildResolvedUrl(const std::string_view host, const std::string_view path, const std::string_view query,
                             const std::string_view fragment) {
  std::string resolved;
  resolved.reserve(host.size() + path.size() + query.size() + fragment.size());
  appendView(resolved, host);
  appendView(resolved, path);
  appendView(resolved, query);
  appendView(resolved, fragment);
  return resolved;
}

std::string normalizeJoinedPath(const std::string_view baseDir, const std::string_view relativePath) {
  std::string combined;
  combined.reserve(baseDir.size() + relativePath.size());
  appendView(combined, baseDir);
  appendView(combined, relativePath);
  return normalizePath(combined);
}
}  // namespace

std::string ensureProtocol(const std::string& url) {
  if (url.find("://") == std::string::npos) {
    return "http://" + url;
  }
  return url;
}

std::string extractHost(const std::string& url) { return std::string(extractHostView(url)); }

std::string extractHostname(const std::string& url) {
  const std::string_view urlView(url);
  const size_t protocolEnd = urlView.find("://");
  size_t hostStart = protocolEnd == std::string::npos ? 0 : protocolEnd + 3;
  if (hostStart >= urlView.size()) {
    return "";
  }

  const size_t authorityEnd = urlView.find_first_of("/?#", hostStart);
  const size_t authorityLimit = authorityEnd == std::string::npos ? urlView.size() : authorityEnd;
  if (hostStart >= authorityLimit) {
    return "";
  }

  if (protocolEnd == std::string::npos && urlView[hostStart] != '[') {
    const size_t colonPos = urlView.find(':', hostStart);
    if (colonPos != std::string::npos && colonPos < authorityLimit) {
      bool digitsOnlyPort = colonPos + 1 < authorityLimit;
      for (size_t i = colonPos + 1; i < authorityLimit; i++) {
        if (urlView[i] < '0' || urlView[i] > '9') {
          digitsOnlyPort = false;
          break;
        }
      }
      if (!digitsOnlyPort) {
        return "";
      }
    }
  }

  const size_t atPos = urlView.find('@', hostStart);
  if (atPos != std::string::npos && atPos < authorityLimit) {
    hostStart = atPos + 1;
  }
  if (hostStart >= authorityLimit) {
    return "";
  }

  if (urlView[hostStart] == '[') {
    const size_t closingBracket = urlView.find(']', hostStart + 1);
    if (closingBracket == std::string::npos || closingBracket >= authorityLimit || closingBracket == hostStart + 1) {
      return "";
    }
    if (closingBracket + 1 < authorityLimit && urlView[closingBracket + 1] != ':') {
      return "";
    }
    return std::string(urlView.substr(hostStart + 1, closingBracket - hostStart - 1));
  }

  const size_t portPos = urlView.find(':', hostStart);
  const size_t hostEnd = (portPos == std::string::npos || portPos >= authorityLimit) ? authorityLimit : portPos;
  return hostEnd > hostStart ? std::string(urlView.substr(hostStart, hostEnd - hostStart)) : "";
}

std::string buildUrl(const std::string& serverUrl, const std::string& path) {
  // If path is already an absolute URL (has protocol), use it directly
  if (path.find("://") != std::string::npos) {
    return path;
  }
  const std::string urlWithProtocol = ensureProtocol(serverUrl);
  const std::string_view strippedUrl = stripQueryAndFragment(urlWithProtocol);
  if (path.empty()) {
    return urlWithProtocol;
  }
  const UrlSuffixParts refParts = splitReference(path);
  if (path.rfind("//", 0) == 0) {
    std::string resolved;
    const std::string_view scheme = extractScheme(urlWithProtocol);
    resolved.reserve(scheme.size() + 1 + path.size());
    appendView(resolved, scheme);
    resolved.push_back(':');
    resolved += path;
    return resolved;
  }
  if (path[0] == '/') {
    // Absolute path - use just the host
    const std::string_view host = extractHostView(strippedUrl);
    const std::string normalizedPath = normalizePath(refParts.path);
    return buildResolvedUrl(host, normalizedPath, refParts.query, refParts.fragment);
  }
  if (path[0] == '?') {
    std::string resolved;
    resolved.reserve(strippedUrl.size() + path.size());
    appendView(resolved, strippedUrl);
    resolved += path;
    return resolved;
  }
  if (path[0] == '#') {
    const std::string_view baseWithoutFragment = stripFragment(urlWithProtocol);
    std::string resolved;
    resolved.reserve(baseWithoutFragment.size() + path.size());
    appendView(resolved, baseWithoutFragment);
    resolved += path;
    return resolved;
  }

  const std::string_view baseHost = extractHostView(strippedUrl);
  std::string_view basePath = strippedUrl.substr(baseHost.size());
  if (basePath.empty()) {
    basePath = std::string_view("/");
  }

  const size_t lastSlash = basePath.find_last_of('/');
  const std::string_view baseDir =
      basePath.back() == '/'
          ? basePath
          : (lastSlash == std::string::npos ? std::string_view("/") : basePath.substr(0, lastSlash + 1));
  const std::string resolvedPath =
      refParts.path.empty() ? std::string(baseDir) : normalizeJoinedPath(baseDir, refParts.path);
  return buildResolvedUrl(baseHost, resolvedPath, refParts.query, refParts.fragment);
}

}  // namespace UrlUtils
