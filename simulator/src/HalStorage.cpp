#include "HalStorage.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <sstream>
#include <system_error>
#include <vector>

HalStorage HalStorage::instance;
HalStorage::HalStorage() {}

namespace {
namespace fs = std::filesystem;

std::string configuredStorageRoot() {
  const char *root = std::getenv("CROSSVI_SIM_SD");
  if (!root || !*root) {
    root = std::getenv("CROSSPOINT_SIM_SD");
  }
  if (!root || !*root) {
    root = std::getenv("CROSSPOINT_EMU_SD");
  }
  return (root && *root) ? std::string(root) : std::string("./fs_");
}

fs::path storageRootPath() {
  std::error_code error;
  fs::path root = fs::weakly_canonical(fs::path(configuredStorageRoot()), error);
  if (!error) {
    return root;
  }
  error.clear();
  root = fs::absolute(fs::path(configuredStorageRoot()), error);
  return error ? fs::path(configuredStorageRoot()).lexically_normal()
               : root.lexically_normal();
}

bool containsUnsafeSegment(const std::string &path) {
  std::stringstream stream(path);
  std::string segment;
  while (std::getline(stream, segment, '/')) {
    if (segment == "..") {
      return true;
    }
  }
  return false;
}

bool isDotDirectoryEntry(const char *name) {
  return name && (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0);
}

bool containsSymlinkComponent(const fs::path &root, const fs::path &target) {
  fs::path relative = target.lexically_relative(root);
  if (relative.empty() && target != root) {
    return true;
  }

  fs::path current = root;
  for (const fs::path &component : relative) {
    if (component == ".") {
      continue;
    }
    if (component == "..") {
      return true;
    }
    current /= component;
    struct stat info {};
    if (::lstat(current.c_str(), &info) == 0) {
      if (S_ISLNK(info.st_mode)) {
        return true;
      }
      continue;
    }
    if (errno == ENOENT) {
      break;
    }
    return true;
  }
  return false;
}

std::string resolveStoragePath(const char *path) {
  std::string logical = path ? std::string(path) : std::string("/");
  if (logical.empty()) {
    logical = "/";
  }
  if (containsUnsafeSegment(logical)) {
    fprintf(stderr, "[SIM] rejected unsafe storage path: %s\n",
            logical.c_str());
    return {};
  }
  while (!logical.empty() && logical.front() == '/') {
    logical.erase(logical.begin());
  }

  const fs::path root = storageRootPath();
  const fs::path full =
      logical.empty() ? root : (root / fs::path(logical)).lexically_normal();
  if (containsSymlinkComponent(root, full)) {
    fprintf(stderr, "[SIM] rejected storage path containing a symlink: %s\n",
            logical.c_str());
    return {};
  }
  return full.string();
}

bool ensureParentDirectories(const std::string &full) {
  const size_t slash = full.find_last_of('/');
  if (slash == std::string::npos) {
    return true;
  }
  const std::string parent = full.substr(0, slash);
  if (parent.empty()) {
    return true;
  }
  for (size_t i = 1; i < parent.size(); ++i) {
    if (parent[i] == '/') {
      ::mkdir(parent.substr(0, i).c_str(), 0777);
    }
  }
  return ::mkdir(parent.c_str(), 0777) == 0 || errno == EEXIST;
}
} // namespace

bool HalStorage::begin() {
  const std::string root = storageRootPath().string();
  for (size_t i = 1; i < root.size(); ++i) {
    if (root[i] == '/') {
      ::mkdir(root.substr(0, i).c_str(), 0777);
    }
  }
  if (::mkdir(root.c_str(), 0777) == 0) {
    initialized = true;
  } else {
    struct stat rootStat {};
    initialized = errno == EEXIST && ::stat(root.c_str(), &rootStat) == 0 &&
                  S_ISDIR(rootStat.st_mode);
  }
  return initialized;
}
bool HalStorage::ready() const { return initialized; }

class HalFile::Impl {
public:
  int fd = -1;
  std::string path;
  DIR *dir = nullptr;
  uint8_t lastError = 0;

  void captureErrno() {
    lastError = errno == 0 ? 1 : static_cast<uint8_t>(errno & 0xff);
  }

  bool open(const char *p, int flags) {
    lastError = 0;
    path = p;
    // The simulator's FsApiConstants.h just includes <fcntl.h> and typedef int
    // oflag_t, so all O_* constants are already native POSIX values — pass them
    // straight through.
    fd = ::open(path.c_str(), flags, 0666);
    if (fd < 0) {
      captureErrno();
      fprintf(stderr, "[SIM] open failed: %s (flags=0x%x errno=%d %s)\n",
              path.c_str(), flags, errno, strerror(errno));
    }
    return fd >= 0;
  }

  bool openAsDir(const char *p) {
    lastError = 0;
    path = p;
    dir = opendir(p);
    if (!dir) {
      captureErrno();
    }
    return dir != nullptr;
  }

  bool isDir() const { return dir != nullptr; }
  bool isOpen() const { return fd >= 0 || dir != nullptr; }
};

HalFile::HalFile() : impl(new Impl()) {}
HalFile::~HalFile() { close(); }
HalFile::HalFile(HalFile &&other) : impl(std::move(other.impl)) {}
HalFile &HalFile::operator=(HalFile &&other) {
  if (this != &other) {
    close();
    impl = std::move(other.impl);
  }
  return *this;
}

void HalFile::flush() {
  if (impl && impl->fd >= 0)
    fsync(impl->fd);
}
bool HalFile::sync() {
  if (!impl || impl->fd < 0)
    return false;
  return fsync(impl->fd) == 0;
}
size_t HalFile::getName(char *name, size_t len) {
  if (!name || len == 0 || !impl || impl->path.empty())
    return 0;
  size_t slash = impl->path.rfind('/');
  std::string fname =
      (slash == std::string::npos) ? impl->path : impl->path.substr(slash + 1);
  if (fname.size() >= len) {
    name[0] = '\0';
    return 0;
  }
  memcpy(name, fname.c_str(), fname.size() + 1);
  return fname.size();
}
size_t HalFile::size() {
  if (!impl || impl->fd < 0)
    return 0;
  off_t cur = lseek(impl->fd, 0, SEEK_CUR);
  off_t end = lseek(impl->fd, 0, SEEK_END);
  lseek(impl->fd, cur, SEEK_SET);
  return end < 0 ? 0 : (size_t)end;
}
size_t HalFile::fileSize() { return size(); }
uint64_t HalFile::fileSize64() { return size(); }
bool HalFile::seek(size_t pos) {
  if (!impl || impl->fd < 0)
    return false;
  return lseek(impl->fd, (off_t)pos, SEEK_SET) >= 0;
}
bool HalFile::seek64(uint64_t pos) {
  if (!impl || impl->fd < 0)
    return false;
  if (pos > static_cast<uint64_t>(std::numeric_limits<off_t>::max()))
    return false;
  return lseek(impl->fd, static_cast<off_t>(pos), SEEK_SET) >= 0;
}
bool HalFile::seekCur(int64_t offset) {
  if (!impl || impl->fd < 0)
    return false;
  return lseek(impl->fd, (off_t)offset, SEEK_CUR) >= 0;
}
bool HalFile::seekSet(size_t offset) {
  if (!impl || impl->fd < 0)
    return false;
  return lseek(impl->fd, (off_t)offset, SEEK_SET) >= 0;
}
int HalFile::available() const {
  if (!impl || impl->fd < 0)
    return 0;
  off_t cur = lseek(impl->fd, 0, SEEK_CUR);
  off_t end = lseek(impl->fd, 0, SEEK_END);
  lseek(impl->fd, cur, SEEK_SET);
  return (int)(end - cur);
}
size_t HalFile::position() const {
  if (!impl || impl->fd < 0)
    return 0;
  off_t pos = lseek(impl->fd, 0, SEEK_CUR);
  return pos < 0 ? 0 : (size_t)pos;
}
uint8_t HalFile::getError() const {
  return impl ? impl->lastError : 0;
}
int HalFile::read(void *buf, size_t count) {
  if (!impl || impl->fd < 0)
    return 0;
  impl->lastError = 0;
  ssize_t n = ::read(impl->fd, buf, count);
  if (n < 0) {
    impl->captureErrno();
    return 0;
  }
  return static_cast<int>(n);
}
int HalFile::read() {
  if (!impl || impl->fd < 0)
    return -1;
  impl->lastError = 0;
  uint8_t c;
  const ssize_t result = ::read(impl->fd, &c, 1);
  if (result < 0) {
    impl->captureErrno();
  }
  return result == 1 ? c : -1;
}
size_t HalFile::write(const void *buf, size_t count) {
  if (!impl || impl->fd < 0)
    return 0;
  impl->lastError = 0;
  ssize_t n = ::write(impl->fd, buf, count);
  if (n < 0) {
    impl->captureErrno();
    return 0;
  }
  return static_cast<size_t>(n);
}
size_t HalFile::write(const uint8_t *buf, size_t count) {
  return write(static_cast<const void *>(buf), count);
}
size_t HalFile::write(uint8_t b) {
  if (!impl || impl->fd < 0)
    return 0;
  impl->lastError = 0;
  if (::write(impl->fd, &b, 1) == 1) {
    return 1;
  }
  impl->captureErrno();
  return 0;
}
bool HalFile::rename(const char *newPath) {
  if (!impl || impl->path.empty()) {
    return false;
  }
  const std::string resolved = resolveStoragePath(newPath);
  if (resolved.empty()) {
    return false;
  }
  close();
  ensureParentDirectories(resolved);
  return ::rename(impl->path.c_str(), resolved.c_str()) == 0;
}
bool HalFile::isDirectory() const { return impl && impl->isDir(); }
void HalFile::rewindDirectory() {
  if (impl && impl->dir)
    rewinddir(impl->dir);
}
bool HalFile::close() {
  if (!impl)
    return true;
  if (impl->dir) {
    closedir(impl->dir);
    impl->dir = nullptr;
  }
  if (impl->fd >= 0) {
    ::close(impl->fd);
    impl->fd = -1;
  }
  return true;
}
HalFile HalFile::openNextFile() {
  if (!impl || !impl->dir)
    return HalFile();
  impl->lastError = 0;
  while (true) {
    errno = 0;
    struct dirent *entry = readdir(impl->dir);
    if (!entry) {
      if (errno != 0) {
        impl->captureErrno();
      }
      return HalFile();
    }
    if (isDotDirectoryEntry(entry->d_name))
      continue;

    std::string childFsPath = impl->path;
    if (childFsPath.back() != '/')
      childFsPath += '/';
    childFsPath += entry->d_name;

    HalFile child;
    struct stat st;
    if (lstat(childFsPath.c_str(), &st) != 0 || S_ISLNK(st.st_mode))
      continue;

    if (S_ISDIR(st.st_mode)) {
      child.impl->openAsDir(childFsPath.c_str());
    } else {
      child.impl->open(childFsPath.c_str(), O_RDONLY);
    }
    return child;
  }
}
bool HalFile::isOpen() const {
  if (!impl)
    return false;
  return impl->isOpen();
}
HalFile::operator bool() const { return isOpen(); }

HalFile HalStorage::open(const char *path, const oflag_t oflag) {
  std::string full = resolveStoragePath(path);
  HalFile f;
  if (full.empty()) {
    return f;
  }
  if ((oflag & O_CREAT) != 0) {
    ensureParentDirectories(full);
  }
  struct stat st;
  if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
    f.impl->openAsDir(full.c_str());
  } else {
    f.impl->open(full.c_str(), oflag);
  }
  return f;
}
bool HalStorage::mkdir(const char *path, const bool /*pFlag*/) {
  std::string full = resolveStoragePath(path);
  if (full.empty()) {
    return false;
  }
  // Create all intermediate directories (mkdir -p semantics).
  for (size_t i = 1; i < full.size(); ++i) {
    if (full[i] == '/') {
      ::mkdir(full.substr(0, i).c_str(),
              0777); // ignore errors (may already exist)
    }
  }
  return ::mkdir(full.c_str(), 0777) == 0 || errno == EEXIST;
}
bool HalStorage::exists(const char *path) {
  std::string full = resolveStoragePath(path);
  if (full.empty()) {
    return false;
  }
  struct stat buffer;
  return (stat(full.c_str(), &buffer) == 0);
}
bool HalStorage::remove(const char *path) {
  std::string full = resolveStoragePath(path);
  if (full.empty()) {
    return false;
  }
  return ::remove(full.c_str()) == 0;
}
bool HalStorage::rename(const char *oldPath, const char *newPath) {
  std::string o = resolveStoragePath(oldPath);
  std::string n = resolveStoragePath(newPath);
  if (o.empty() || n.empty()) {
    return false;
  }
  ensureParentDirectories(n);
  return ::rename(o.c_str(), n.c_str()) == 0;
}
static bool removeDirRecursive(const std::string &full) {
  struct stat rootInfo {};
  if (lstat(full.c_str(), &rootInfo) != 0) {
    return false;
  }
  if (S_ISLNK(rootInfo.st_mode) || !S_ISDIR(rootInfo.st_mode)) {
    return ::remove(full.c_str()) == 0;
  }
  DIR *d = opendir(full.c_str());
  if (!d)
    return false;
  bool success = true;
  struct dirent *entry;
  while ((entry = readdir(d)) != nullptr) {
    if (isDotDirectoryEntry(entry->d_name))
      continue;
    std::string child = full + "/" + entry->d_name;
    struct stat st;
    if (lstat(child.c_str(), &st) != 0) {
      success = false;
    } else if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
      success = removeDirRecursive(child) && success;
    } else {
      success = ::remove(child.c_str()) == 0 && success;
    }
  }
  closedir(d);
  return success && ::rmdir(full.c_str()) == 0;
}

bool HalStorage::rmdir(const char *path) {
  std::string full = resolveStoragePath(path);
  if (full.empty()) {
    return false;
  }
  return removeDirRecursive(full);
}
bool HalStorage::removeDir(const char *path) {
  std::string full = resolveStoragePath(path);
  if (full.empty()) {
    return false;
  }
  return removeDirRecursive(full);
}

String HalStorage::readFile(const char *path) {
  HalFile f = open(path, O_RDONLY);
  if (!f)
    return String("");
  size_t s = f.size();
  std::string content(s, '\0');
  f.read((void *)content.data(), s);
  return String(content);
}
bool HalStorage::readFileToStream(const char *path, Print &out,
                                  size_t chunkSize) {
  HalFile f = open(path, O_RDONLY);
  if (!f)
    return false;
  std::vector<char> buf(chunkSize);
  int n;
  while ((n = f.read(buf.data(), chunkSize)) > 0) {
    out.write(reinterpret_cast<const uint8_t *>(buf.data()), n);
  }
  return true;
}
size_t HalStorage::readFileToBuffer(const char *path, char *buffer,
                                    size_t bufferSize, size_t maxBytes) {
  HalFile f = open(path, O_RDONLY);
  if (!f)
    return 0;
  size_t toRead = bufferSize - 1;
  if (maxBytes > 0 && maxBytes < toRead)
    toRead = maxBytes;
  int n = f.read(buffer, toRead);
  if (n < 0)
    n = 0;
  buffer[n] = '\0';
  return n;
}
bool HalStorage::writeFile(const char *path, const String &content) {
  HalFile f = open(path, O_WRONLY | O_CREAT | O_TRUNC);
  if (!f)
    return false;
  f.write(content.c_str(), content.length());
  return true;
}
bool HalStorage::ensureDirectoryExists(const char *path) { return mkdir(path); }

bool HalStorage::openFileForRead(const char *moduleName, const char *path,
                                 HalFile &file) {
  file = open(path, O_RDONLY);
  return file.isOpen();
}
bool HalStorage::openFileForRead(const char *moduleName,
                                 const std::string &path, HalFile &file) {
  return openFileForRead(moduleName, path.c_str(), file);
}
bool HalStorage::openFileForRead(const char *moduleName, const String &path,
                                 HalFile &file) {
  return openFileForRead(moduleName, path.c_str(), file);
}
bool HalStorage::openFileForWrite(const char *moduleName, const char *path,
                                  HalFile &file) {
  file = open(path, O_WRONLY | O_CREAT | O_TRUNC);
  return file.isOpen();
}
bool HalStorage::openFileForWrite(const char *moduleName,
                                  const std::string &path, HalFile &file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}
bool HalStorage::openFileForWrite(const char *moduleName, const String &path,
                                  HalFile &file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

std::vector<String> HalStorage::listFiles(const char *path, int maxFiles) {
  std::vector<String> result;
  std::string full = resolveStoragePath(path);
  if (full.empty()) {
    return result;
  }
  DIR *dir = opendir(full.c_str());
  if (!dir)
    return result;
  struct dirent *entry;
  while ((entry = readdir(dir)) != nullptr && (int)result.size() < maxFiles) {
    if (isDotDirectoryEntry(entry->d_name))
      continue;
    const std::string child = full + "/" + entry->d_name;
    struct stat info {};
    if (lstat(child.c_str(), &info) != 0 || S_ISLNK(info.st_mode))
      continue;
    result.push_back(String(entry->d_name));
  }
  closedir(dir);
  return result;
}
