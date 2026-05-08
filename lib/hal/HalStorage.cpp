#define HAL_STORAGE_IMPL
#include "HalStorage.h"

#if CROSSPOINT_EMULATED
#include <Logging.h>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

HalStorage HalStorage::instance;

namespace {
fs::path rootPath() {
  const char* env = std::getenv("CROSSPOINT_EMU_SD");
  return env ? fs::path(env) : fs::current_path() / "emu_sd";
}

fs::path mapPath(const char* path) {
  std::string p = path ? path : "/";
  while (!p.empty() && p.front() == '/') p.erase(p.begin());
  return rootPath() / p;
}
}  // namespace

HalStorage::HalStorage() {}
bool HalStorage::begin() {
  fs::create_directories(rootPath() / ".crosspoint");
  initialized = true;
  return true;
}
bool HalStorage::ready() const { return initialized; }
std::vector<String> HalStorage::listFiles(const char* path, int maxFiles) {
  std::vector<String> out;
  const fs::path dir = mapPath(path);
  if (!fs::exists(dir) || !fs::is_directory(dir)) return out;
  for (const auto& entry : fs::directory_iterator(dir)) {
    if (static_cast<int>(out.size()) >= maxFiles) break;
    out.emplace_back(entry.path().filename().string());
  }
  return out;
}
String HalStorage::readFile(const char* path) {
  std::ifstream in(mapPath(path), std::ios::binary);
  if (!in) return {};
  return String(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()));
}
bool HalStorage::readFileToStream(const char* path, Print& out, size_t chunkSize) {
  std::ifstream in(mapPath(path), std::ios::binary);
  if (!in) return false;
  std::vector<uint8_t> buf(chunkSize);
  while (in) {
    in.read(reinterpret_cast<char*>(buf.data()), buf.size());
    const auto n = in.gcount();
    if (n > 0) out.write(buf.data(), static_cast<size_t>(n));
  }
  return true;
}
size_t HalStorage::readFileToBuffer(const char* path, char* buffer, size_t bufferSize, size_t maxBytes) {
  if (!buffer || bufferSize == 0) return 0;
  std::ifstream in(mapPath(path), std::ios::binary);
  if (!in) {
    buffer[0] = '\0';
    return 0;
  }
  const size_t limit = std::min(bufferSize - 1, maxBytes ? maxBytes : bufferSize - 1);
  in.read(buffer, limit);
  const size_t n = static_cast<size_t>(in.gcount());
  buffer[n] = '\0';
  return n;
}
bool HalStorage::writeFile(const char* path, const String& content) {
  fs::create_directories(mapPath(path).parent_path());
  std::ofstream out(mapPath(path), std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(content.c_str(), content.length());
  return true;
}
bool HalStorage::ensureDirectoryExists(const char* path) {
  return fs::create_directories(mapPath(path)) || fs::exists(mapPath(path));
}

class HalFile::Impl {
 public:
  fs::path path;
  std::fstream stream;
  std::vector<fs::directory_entry> entries;
  size_t entryIndex = 0;
  bool directory = false;
  bool readable = false;
  bool writable = false;
};

HalFile::HalFile() = default;
HalFile::HalFile(std::unique_ptr<Impl> impl) : impl(std::move(impl)) {}
HalFile::~HalFile() = default;
HalFile::HalFile(HalFile&&) = default;
HalFile& HalFile::operator=(HalFile&&) = default;

HalFile HalStorage::open(const char* path, const oflag_t oflag) {
  auto impl = std::make_unique<HalFile::Impl>();
  impl->path = mapPath(path);
  impl->directory = fs::is_directory(impl->path);
  if (impl->directory) {
    for (const auto& entry : fs::directory_iterator(impl->path)) impl->entries.push_back(entry);
    return HalFile(std::move(impl));
  }
  fs::create_directories(impl->path.parent_path());
  std::ios::openmode mode = std::ios::binary;
  if (oflag & O_RDWR)
    mode |= std::ios::in | std::ios::out;
  else if (oflag & O_WRONLY)
    mode |= std::ios::out;
  else
    mode |= std::ios::in;
  if (oflag & O_TRUNC) mode |= std::ios::trunc;
  if (oflag & O_CREAT) mode |= std::ios::out;
  impl->readable = (mode & std::ios::in) != 0;
  impl->writable = (mode & std::ios::out) != 0;
  impl->stream.open(impl->path, mode);
  return HalFile(std::move(impl));
}
bool HalStorage::mkdir(const char* path, const bool) {
  return fs::create_directories(mapPath(path)) || fs::exists(mapPath(path));
}
bool HalStorage::exists(const char* path) { return fs::exists(mapPath(path)); }
bool HalStorage::remove(const char* path) { return fs::remove(mapPath(path)); }
bool HalStorage::rename(const char* oldPath, const char* newPath) {
  fs::create_directories(mapPath(newPath).parent_path());
  std::error_code ec;
  fs::rename(mapPath(oldPath), mapPath(newPath), ec);
  return !ec;
}
bool HalStorage::rmdir(const char* path) { return fs::remove(mapPath(path)); }
bool HalStorage::openFileForRead(const char*, const char* path, HalFile& file) {
  file = open(path, O_RDONLY);
  return file;
}
bool HalStorage::openFileForRead(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}
bool HalStorage::openFileForRead(const char* moduleName, const String& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}
bool HalStorage::openFileForWrite(const char*, const char* path, HalFile& file) {
  file = open(path, O_CREAT | O_TRUNC | O_WRONLY);
  return file;
}
bool HalStorage::openFileForWrite(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}
bool HalStorage::openFileForWrite(const char* moduleName, const String& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}
bool HalStorage::removeDir(const char* path) { return fs::remove_all(mapPath(path)) > 0; }
void HalFile::flush() {
  if (impl) impl->stream.flush();
}
size_t HalFile::getName(char* name, size_t len) {
  if (!impl || !name || len == 0) return 0;
  const auto s = impl->path.filename().string();
  snprintf(name, len, "%s", s.c_str());
  return std::min(len - 1, s.size());
}
size_t HalFile::size() { return fileSize(); }
size_t HalFile::fileSize() {
  if (!impl || impl->directory) return 0;
  impl->stream.clear();
  if (impl->writable) impl->stream.flush();
  if (fs::exists(impl->path)) return fs::file_size(impl->path);
  return 0;
}
uint64_t HalFile::fileSize64() { return fileSize(); }
bool HalFile::seek(size_t pos) { return seekSet(pos); }
bool HalFile::seek64(uint64_t pos) { return seekSet(static_cast<size_t>(pos)); }
bool HalFile::seekCur(int64_t offset) {
  if (!impl) return false;
  impl->stream.clear();
  if (impl->readable) impl->stream.seekg(offset, std::ios::cur);
  if (impl->writable) impl->stream.seekp(offset, std::ios::cur);
  return !impl->stream.fail();
}
bool HalFile::seekSet(size_t offset) {
  if (!impl) return false;
  impl->stream.clear();
  if (impl->readable) impl->stream.seekg(offset);
  if (impl->writable) impl->stream.seekp(offset);
  return !impl->stream.fail();
}
int HalFile::available() const {
  if (!impl || impl->directory || !impl->readable) return 0;
  impl->stream.clear();
  auto pos = impl->stream.tellg();
  impl->stream.seekg(0, std::ios::end);
  auto end = impl->stream.tellg();
  impl->stream.seekg(pos);
  if (pos < 0 || end < pos) return 0;
  return static_cast<int>(end - pos);
}
size_t HalFile::position() const {
  if (!impl) return 0;
  impl->stream.clear();
  const auto pos = impl->writable ? impl->stream.tellp() : impl->stream.tellg();
  return pos < 0 ? 0 : static_cast<size_t>(pos);
}
int HalFile::read(void* buf, size_t count) {
  if (!impl || !buf || !impl->readable) return -1;
  impl->stream.clear();
  impl->stream.read(static_cast<char*>(buf), count);
  return static_cast<int>(impl->stream.gcount());
}
int HalFile::read() {
  char c = 0;
  return read(&c, 1) == 1 ? static_cast<unsigned char>(c) : -1;
}
size_t HalFile::write(const void* buf, size_t count) {
  if (!impl || !buf || !impl->writable) return 0;
  impl->stream.clear();
  impl->stream.write(static_cast<const char*>(buf), count);
  return impl->stream.fail() ? 0 : count;
}
size_t HalFile::write(const uint8_t* buf, size_t count) { return write(static_cast<const void*>(buf), count); }
size_t HalFile::write(uint8_t b) { return write(&b, 1); }
bool HalFile::rename(const char* newPath) {
  if (!impl) return false;
  std::error_code ec;
  fs::rename(impl->path, mapPath(newPath), ec);
  if (!ec) impl->path = mapPath(newPath);
  return !ec;
}
bool HalFile::isDirectory() const { return impl && impl->directory; }
void HalFile::rewindDirectory() {
  if (impl) impl->entryIndex = 0;
}
bool HalFile::close() {
  if (!impl) return false;
  if (impl->stream.is_open()) impl->stream.close();
  impl.reset();
  return true;
}
HalFile HalFile::openNextFile() {
  if (!impl || impl->entryIndex >= impl->entries.size()) return {};
  auto child = std::make_unique<Impl>();
  child->path = impl->entries[impl->entryIndex++].path();
  child->directory = fs::is_directory(child->path);
  if (child->directory) {
    for (const auto& entry : fs::directory_iterator(child->path)) child->entries.push_back(entry);
  } else {
    child->stream.open(child->path, std::ios::binary | std::ios::in);
    child->readable = true;
  }
  return HalFile(std::move(child));
}
bool HalFile::isOpen() const { return impl && (impl->directory || impl->stream.is_open()); }
HalFile::operator bool() const { return isOpen(); }

#else

#include <FS.h>  // need to be included before SdFat.h for compatibility with FS.h's File class
#include <Logging.h>
#include <SDCardManager.h>

#include <cassert>

#define SDCard SDCardManager::getInstance()

HalStorage HalStorage::instance;

HalStorage::HalStorage() {
  storageMutex = xSemaphoreCreateMutex();
  assert(storageMutex != nullptr);
}

// begin() and ready() are only called from setup, no need to acquire mutex for them

bool HalStorage::begin() { return SDCard.begin(); }

bool HalStorage::ready() const { return SDCard.ready(); }

// For the rest of the methods, we acquire the mutex to ensure thread safety

class HalStorage::StorageLock {
 public:
  StorageLock() { xSemaphoreTake(HalStorage::getInstance().storageMutex, portMAX_DELAY); }
  ~StorageLock() { xSemaphoreGive(HalStorage::getInstance().storageMutex); }
};

#define HAL_STORAGE_WRAPPED_CALL(method, ...) \
  HalStorage::StorageLock lock;               \
  return SDCard.method(__VA_ARGS__);

std::vector<String> HalStorage::listFiles(const char* path, int maxFiles) {
  HAL_STORAGE_WRAPPED_CALL(listFiles, path, maxFiles);
}

String HalStorage::readFile(const char* path) { HAL_STORAGE_WRAPPED_CALL(readFile, path); }

bool HalStorage::readFileToStream(const char* path, Print& out, size_t chunkSize) {
  HAL_STORAGE_WRAPPED_CALL(readFileToStream, path, out, chunkSize);
}

size_t HalStorage::readFileToBuffer(const char* path, char* buffer, size_t bufferSize, size_t maxBytes) {
  HAL_STORAGE_WRAPPED_CALL(readFileToBuffer, path, buffer, bufferSize, maxBytes);
}

bool HalStorage::writeFile(const char* path, const String& content) {
  HAL_STORAGE_WRAPPED_CALL(writeFile, path, content);
}

bool HalStorage::ensureDirectoryExists(const char* path) { HAL_STORAGE_WRAPPED_CALL(ensureDirectoryExists, path); }

class HalFile::Impl {
 public:
  Impl(FsFile&& fsFile) : file(std::move(fsFile)) {}
  FsFile file;
};

HalFile::HalFile() = default;

HalFile::HalFile(std::unique_ptr<Impl> impl) : impl(std::move(impl)) {}

HalFile::~HalFile() = default;

HalFile::HalFile(HalFile&&) = default;

HalFile& HalFile::operator=(HalFile&&) = default;

HalFile HalStorage::open(const char* path, const oflag_t oflag) {
  StorageLock lock;  // ensure thread safety for the duration of this function
  return HalFile(std::make_unique<HalFile::Impl>(SDCard.open(path, oflag)));
}

bool HalStorage::mkdir(const char* path, const bool pFlag) { HAL_STORAGE_WRAPPED_CALL(mkdir, path, pFlag); }

bool HalStorage::exists(const char* path) { HAL_STORAGE_WRAPPED_CALL(exists, path); }

bool HalStorage::remove(const char* path) { HAL_STORAGE_WRAPPED_CALL(remove, path); }
bool HalStorage::rename(const char* oldPath, const char* newPath) {
  HAL_STORAGE_WRAPPED_CALL(rename, oldPath, newPath);
}

bool HalStorage::rmdir(const char* path) { HAL_STORAGE_WRAPPED_CALL(rmdir, path); }

bool HalStorage::openFileForRead(const char* moduleName, const char* path, HalFile& file) {
  StorageLock lock;  // ensure thread safety for the duration of this function
  FsFile fsFile;
  bool ok = SDCard.openFileForRead(moduleName, path, fsFile);
  file = HalFile(std::make_unique<HalFile::Impl>(std::move(fsFile)));
  return ok;
}

bool HalStorage::openFileForRead(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForRead(const char* moduleName, const String& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const char* path, HalFile& file) {
  StorageLock lock;  // ensure thread safety for the duration of this function
  FsFile fsFile;
  bool ok = SDCard.openFileForWrite(moduleName, path, fsFile);
  file = HalFile(std::make_unique<HalFile::Impl>(std::move(fsFile)));
  return ok;
}

bool HalStorage::openFileForWrite(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const String& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::removeDir(const char* path) { HAL_STORAGE_WRAPPED_CALL(removeDir, path); }

// HalFile implementation
// Allow doing file operations while ensuring thread safety via HalStorage's mutex.
// Please keep the list below in sync with the HalFile.h header

#define HAL_FILE_WRAPPED_CALL(method, ...) \
  HalStorage::StorageLock lock;            \
  assert(impl != nullptr);                 \
  return impl->file.method(__VA_ARGS__);

#define HAL_FILE_FORWARD_CALL(method, ...) \
  assert(impl != nullptr);                 \
  return impl->file.method(__VA_ARGS__);

void HalFile::flush() { HAL_FILE_WRAPPED_CALL(flush, ); }
size_t HalFile::getName(char* name, size_t len) { HAL_FILE_WRAPPED_CALL(getName, name, len); }
size_t HalFile::size() { HAL_FILE_FORWARD_CALL(size, ); }              // already thread-safe, no need to wrap
size_t HalFile::fileSize() { HAL_FILE_FORWARD_CALL(fileSize, ); }      // already thread-safe, no need to wrap
uint64_t HalFile::fileSize64() { HAL_FILE_FORWARD_CALL(fileSize, ); }  // already thread-safe, no need to wrap
bool HalFile::seek(size_t pos) { HAL_FILE_WRAPPED_CALL(seekSet, pos); }
bool HalFile::seek64(uint64_t pos) { HAL_FILE_WRAPPED_CALL(seekSet, pos); }
bool HalFile::seekCur(int64_t offset) { HAL_FILE_WRAPPED_CALL(seekCur, offset); }
bool HalFile::seekSet(size_t offset) { HAL_FILE_WRAPPED_CALL(seekSet, offset); }
int HalFile::available() const { HAL_FILE_WRAPPED_CALL(available, ); }
size_t HalFile::position() const { HAL_FILE_WRAPPED_CALL(position, ); }
int HalFile::read(void* buf, size_t count) { HAL_FILE_WRAPPED_CALL(read, buf, count); }
int HalFile::read() { HAL_FILE_WRAPPED_CALL(read, ); }
size_t HalFile::write(const void* buf, size_t count) { HAL_FILE_WRAPPED_CALL(write, buf, count); }
size_t HalFile::write(const uint8_t* buf, size_t count) { return write(static_cast<const void*>(buf), count); }
size_t HalFile::write(uint8_t b) { HAL_FILE_WRAPPED_CALL(write, b); }
bool HalFile::rename(const char* newPath) { HAL_FILE_WRAPPED_CALL(rename, newPath); }
bool HalFile::isDirectory() const { HAL_FILE_FORWARD_CALL(isDirectory, ); }  // already thread-safe, no need to wrap
void HalFile::rewindDirectory() { HAL_FILE_WRAPPED_CALL(rewindDirectory, ); }
bool HalFile::close() { HAL_FILE_WRAPPED_CALL(close, ); }
HalFile HalFile::openNextFile() {
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  return HalFile(std::make_unique<Impl>(impl->file.openNextFile()));
}
bool HalFile::isOpen() const { return impl != nullptr && impl->file.isOpen(); }  // already thread-safe, no need to wrap
HalFile::operator bool() const { return isOpen(); }
#endif
