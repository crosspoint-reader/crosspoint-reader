#pragma once

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace sim_http_fetch {

struct Response {
  int statusCode = 0;
  int curlExitCode = 0;
  std::string body;
};

inline bool startsWith(const std::string &value, const char *prefix) {
  return value.rfind(prefix, 0) == 0;
}

inline std::string shellQuote(const std::string &value) {
  std::string out = "'";
  for (char c : value) {
    if (c == '\'')
      out += "'\\''";
    else
      out += c;
  }
  out += "'";
  return out;
}

inline int hexValue(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

inline std::string urlDecode(std::string value) {
  std::string out;
  out.reserve(value.size());
  for (size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '%' && i + 2 < value.size()) {
      int hi = hexValue(value[i + 1]);
      int lo = hexValue(value[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    out.push_back(value[i]);
  }
  return out;
}

inline std::string basenameFromUrl(const std::string &url) {
  size_t end = url.find_first_of("?#");
  std::string path = url.substr(0, end == std::string::npos ? url.size() : end);
  size_t slash = path.find_last_of('/');
  return urlDecode(slash == std::string::npos ? path : path.substr(slash + 1));
}

inline bool readFile(const std::string &path, std::string &out) {
  FILE *file = std::fopen(path.c_str(), "rb");
  if (!file)
    return false;

  out.clear();
  std::array<char, 8192> buffer{};
  while (true) {
    size_t n = std::fread(buffer.data(), 1, buffer.size(), file);
    if (n > 0)
      out.append(buffer.data(), n);
    if (n < buffer.size()) {
      if (std::ferror(file)) {
        std::fclose(file);
        return false;
      }
      break;
    }
  }

  std::fclose(file);
  return true;
}

inline bool isDirectory(const std::string &path) {
  struct stat st {};
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

inline bool findFileByBasename(const std::string &dir, const std::string &name,
                               std::string &outPath, int depth = 0) {
  if (depth > 5)
    return false;

  DIR *handle = opendir(dir.c_str());
  if (!handle)
    return false;

  while (dirent *entry = readdir(handle)) {
    std::string entryName = entry->d_name;
    if (entryName == "." || entryName == "..")
      continue;

    std::string path = dir;
    if (!path.empty() && path.back() != '/')
      path += '/';
    path += entryName;

    if (entryName == name) {
      outPath = path;
      closedir(handle);
      return true;
    }

    if (isDirectory(path) && findFileByBasename(path, name, outPath, depth + 1)) {
      closedir(handle);
      return true;
    }
  }

  closedir(handle);
  return false;
}

inline bool fetchFromFileUrl(const std::string &url, Response &out) {
  if (!startsWith(url, "file://"))
    return false;

  std::string path = urlDecode(url.substr(strlen("file://")));
  if (startsWith(path, "localhost/"))
    path = path.substr(strlen("localhost"));

  if (readFile(path, out.body)) {
    out.statusCode = 200;
  } else {
    out.body.clear();
    out.statusCode = 404;
  }
  return true;
}

inline bool fetchFromMockRoot(const std::string &url, Response &out) {
  const char *root = std::getenv("CROSSPOINT_SIM_HTTP_MOCK_ROOT");
  if (!root || root[0] == '\0')
    return false;

  std::string name = basenameFromUrl(url);
  if (name.empty() || name == "." || name == ".." ||
      name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
    return false;
  }

  std::string path = root;
  if (!path.empty() && path.back() != '/')
    path += '/';
  path += name;

  if (!readFile(path, out.body)) {
    std::string nestedPath;
    if (!findFileByBasename(root, name, nestedPath) || !readFile(nestedPath, out.body))
      return false;
  }

  if (!out.body.empty()) {
    out.statusCode = 200;
    return true;
  }
  return false;
}

inline bool fetchWithCurl(const std::string &url, const char *method,
                          const std::map<std::string, std::string> &headers,
                          const std::string &basicAuth, const char *body,
                          Response &out) {
  char tmpTemplate[] = "/tmp/crosspoint-sim-http-XXXXXX";
  int fd = mkstemp(tmpTemplate);
  if (fd < 0)
    return false;
  close(fd);

  std::string cmd = "curl -L -sS --connect-timeout 10 --max-time 60 -o ";
  cmd += shellQuote(tmpTemplate);
  cmd += " -w '%{http_code}'";
  if (method && std::string(method) != "GET")
    cmd += " -X " + shellQuote(method);
  for (const auto &header : headers) {
    cmd += " -H " + shellQuote(header.first + ": " + header.second);
  }
  if (!basicAuth.empty())
    cmd += " -u " + shellQuote(basicAuth);
  if (body)
    cmd += " --data-binary " + shellQuote(body);
  cmd += " " + shellQuote(url);

  FILE *pipe = popen(cmd.c_str(), "r");
  if (!pipe) {
    unlink(tmpTemplate);
    return false;
  }

  std::string statusText;
  std::array<char, 64> statusBuffer{};
  while (fgets(statusBuffer.data(), static_cast<int>(statusBuffer.size()), pipe)) {
    statusText += statusBuffer.data();
  }
  const int rc = pclose(pipe);
  if (rc >= 0 && WIFEXITED(rc))
    out.curlExitCode = WEXITSTATUS(rc);

  bool readOk = readFile(tmpTemplate, out.body);
  unlink(tmpTemplate);
  if (!readOk)
    out.body.clear();

  out.statusCode = std::atoi(statusText.c_str());
  return rc == 0 || out.statusCode > 0 || !out.body.empty();
}

inline bool fetch(const std::string &url, const char *method,
                  const std::map<std::string, std::string> &headers,
                  const std::string &basicAuth, const char *body, Response &out) {
  out = Response{};
  if (fetchFromMockRoot(url, out))
    return true;
  if (fetchFromFileUrl(url, out))
    return true;
  return fetchWithCurl(url, method, headers, basicAuth, body, out);
}

} // namespace sim_http_fetch
