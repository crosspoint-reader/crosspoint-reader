#include "HalStorage.h"

#include <cstdio>
#include <fstream>

struct HalFile::Impl {
  std::fstream stream;
  bool open = false;
};

HalFile::HalFile() = default;
HalFile::HalFile(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
HalFile::~HalFile() = default;
HalFile::HalFile(HalFile&&) noexcept = default;
HalFile& HalFile::operator=(HalFile&&) noexcept = default;

void HalFile::flush() {
  if (impl_ && impl_->open) impl_->stream.flush();
}

size_t HalFile::size() {
  if (!impl_ || !impl_->open) return 0;
  const auto cur = impl_->stream.tellg();
  impl_->stream.seekg(0, std::ios::end);
  const auto end = impl_->stream.tellg();
  impl_->stream.seekg(cur);
  return static_cast<size_t>(end);
}

bool HalFile::isOpen() const { return impl_ && impl_->open; }

int HalFile::available() const {
  if (!impl_ || !impl_->open) return 0;
  const auto cur = impl_->stream.tellg();
  impl_->stream.seekg(0, std::ios::end);
  const auto end = impl_->stream.tellg();
  impl_->stream.seekg(cur);
  return static_cast<int>(end - cur);
}

size_t HalFile::position() const { return impl_ ? static_cast<size_t>(impl_->stream.tellg()) : 0; }

bool HalFile::seek(size_t pos) {
  if (!impl_ || !impl_->open) return false;
  impl_->stream.seekg(static_cast<std::streamoff>(pos));
  impl_->stream.seekp(static_cast<std::streamoff>(pos));
  return true;
}

int HalFile::read(void* buf, size_t count) {
  if (!impl_ || !impl_->open) return 0;
  impl_->stream.read(static_cast<char*>(buf), static_cast<std::streamsize>(count));
  return static_cast<int>(impl_->stream.gcount());
}

size_t HalFile::write(const void* buf, size_t count) {
  if (!impl_ || !impl_->open) return 0;
  impl_->stream.write(static_cast<const char*>(buf), static_cast<std::streamsize>(count));
  return count;
}

size_t HalFile::write(uint8_t b) { return write(&b, 1); }

bool HalFile::close() {
  if (impl_ && impl_->open) {
    impl_->stream.close();
    impl_->open = false;
  }
  return true;
}

HalFile::operator bool() const { return isOpen(); }

bool HalStorage::exists(const char* path) {
  std::ifstream f(path);
  return f.good();
}

bool HalStorage::remove(const char* path) { return std::remove(path) == 0; }

bool HalStorage::openFileForRead(const char* /*moduleName*/, const std::string& path, HalFile& file) {
  auto impl = std::make_unique<HalFile::Impl>();
  impl->stream.open(path, std::ios::in | std::ios::binary);
  if (!impl->stream.is_open()) return false;
  impl->open = true;
  file = HalFile(std::move(impl));
  return true;
}

bool HalStorage::openFileForWrite(const char* /*moduleName*/, const std::string& path, HalFile& file) {
  auto impl = std::make_unique<HalFile::Impl>();
  impl->stream.open(path, std::ios::out | std::ios::binary | std::ios::trunc);
  if (!impl->stream.is_open()) return false;
  impl->open = true;
  file = HalFile(std::move(impl));
  return true;
}
