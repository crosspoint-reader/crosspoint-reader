#include <HalStorage.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// In-memory stand-in for the SD-backed HalStorage/HalFile used by the firmware.
// ContentOpfParser streams manifest items to a temp file and reads them back to
// resolve spine itemrefs; this stub emulates that with a process-local map of
// path -> byte buffer so the host test exercises the real parser end-to-end.

namespace {

using FileData = std::vector<uint8_t>;
std::unordered_map<std::string, std::shared_ptr<FileData>>& fileStore() {
  static std::unordered_map<std::string, std::shared_ptr<FileData>> store;
  return store;
}

}  // namespace

class HalFile::Impl {
 public:
  std::shared_ptr<FileData> data = std::make_shared<FileData>();
  size_t pos = 0;
  bool open = false;
};

HalStorage HalStorage::instance;

HalStorage::HalStorage() = default;

bool HalStorage::exists(const char* path) { return fileStore().find(path) != fileStore().end(); }

bool HalStorage::remove(const char* path) { return fileStore().erase(path) > 0; }

bool HalStorage::openFileForRead(const char* moduleName, const char* path, HalFile& file) {
  (void)moduleName;
  auto it = fileStore().find(path);
  if (it == fileStore().end()) {
    return false;
  }
  auto impl = std::make_unique<HalFile::Impl>();
  impl->data = it->second;
  impl->pos = 0;
  impl->open = true;
  file = HalFile(std::move(impl));
  return true;
}

bool HalStorage::openFileForRead(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForRead(const char* moduleName, const String& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const char* path, HalFile& file) {
  (void)moduleName;
  auto impl = std::make_unique<HalFile::Impl>();
  fileStore()[path] = impl->data;
  impl->pos = 0;
  impl->open = true;
  file = HalFile(std::move(impl));
  return true;
}

bool HalStorage::openFileForWrite(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const String& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

HalFile::HalFile() = default;
HalFile::HalFile(std::unique_ptr<Impl> impl) : impl(std::move(impl)) {}
HalFile::~HalFile() = default;
HalFile::HalFile(HalFile&&) = default;
HalFile& HalFile::operator=(HalFile&&) = default;

size_t HalFile::write(uint8_t b) {
  if (!impl || !impl->open) {
    return 0;
  }
  if (impl->pos >= impl->data->size()) {
    impl->data->resize(impl->pos + 1);
  }
  (*impl->data)[impl->pos] = b;
  impl->pos++;
  return 1;
}

size_t HalFile::write(const void* buf, size_t count) {
  if (!impl || !impl->open) {
    return 0;
  }
  const auto* p = static_cast<const uint8_t*>(buf);
  if (impl->pos + count > impl->data->size()) {
    impl->data->resize(impl->pos + count);
  }
  std::memcpy(impl->data->data() + impl->pos, p, count);
  impl->pos += count;
  return count;
}

int HalFile::read(void* buf, size_t count) {
  if (!impl || !impl->open) {
    return -1;
  }
  const size_t remaining = impl->data->size() - std::min(impl->pos, impl->data->size());
  const size_t n = std::min(count, remaining);
  if (n > 0) {
    std::memcpy(buf, impl->data->data() + impl->pos, n);
    impl->pos += n;
  }
  return static_cast<int>(n);
}

int HalFile::read() {
  uint8_t b = 0;
  const int n = read(&b, 1);
  return n == 1 ? static_cast<int>(b) : -1;
}

bool HalFile::seek(size_t pos) {
  if (!impl || !impl->open) {
    return false;
  }
  impl->pos = pos;
  return true;
}

size_t HalFile::position() const { return impl ? impl->pos : 0; }

int HalFile::available() const {
  if (!impl || !impl->open) {
    return 0;
  }
  return static_cast<int>(impl->data->size() - std::min(impl->pos, impl->data->size()));
}

bool HalFile::close() {
  if (!impl) {
    return false;
  }
  impl->open = false;
  return true;
}

bool HalFile::isOpen() const { return impl && impl->open; }

HalFile::operator bool() const { return isOpen(); }
