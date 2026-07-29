#include "SdFatFS.h"

#include <FSImpl.h>
#include <HalStorage.h>
#include <Logging.h>

#include <memory>
#include <string>

namespace {

class HalFileImpl final : public fs::FileImpl {
 public:
  HalFileImpl(HalFile&& file, std::string path) : file_(std::move(file)), path_(std::move(path)) {
    const auto slash = path_.find_last_of('/');
    name_ = (slash == std::string::npos) ? path_ : path_.substr(slash + 1);
  }

  size_t write(const uint8_t* buf, size_t size) override { return file_.write(buf, size); }

  size_t read(uint8_t* buf, size_t size) override {
    const int n = file_.read(buf, size);
    return n < 0 ? 0 : static_cast<size_t>(n);
  }

  void flush() override { file_.flush(); }

  bool seek(uint32_t pos, SeekMode mode) override {
    switch (mode) {
      case SeekSet:
        return file_.seek(pos);
      case SeekCur:
        return file_.seekCur(static_cast<int64_t>(pos));
      case SeekEnd:
        return file_.seek(file_.fileSize() - pos);
    }
    return false;
  }

  size_t position() const override { return file_.position(); }
  size_t size() const override { return const_cast<HalFile&>(file_).fileSize(); }
  bool setBufferSize(size_t) override { return false; }
  void close() override { file_.close(); }
  time_t getLastWrite() override { return 0; }
  const char* path() const override { return path_.c_str(); }
  const char* name() const override { return name_.c_str(); }
  boolean isDirectory(void) override { return file_.isDirectory(); }
  // Directory iteration is not bridged — CrossPoint code lists directories
  // through HalStorage; fs::FS consumers here only read single files.
  fs::FileImplPtr openNextFile(const char*) override { return nullptr; }
  boolean seekDir(long) override { return false; }
  String getNextFileName(void) override { return String(); }
  String getNextFileName(bool*) override { return String(); }
  void rewindDirectory(void) override {}
  operator bool() override { return file_.isOpen(); }

 private:
  HalFile file_;
  std::string path_;
  std::string name_;
};

class SdFatFSImpl final : public fs::FSImpl {
 public:
  fs::FileImplPtr open(const char* path, const char* mode, const bool create) override {
    (void)create;
    HalFile file;
    const bool forWrite = mode && (mode[0] == 'w' || mode[0] == 'a');
    const bool ok = forWrite ? Storage.openFileForWrite("FSFS", path, file)
                             : Storage.openFileForRead("FSFS", path, file);
    if (!ok) return nullptr;
    return std::make_shared<HalFileImpl>(std::move(file), std::string(path));
  }

  bool exists(const char* path) override { return Storage.exists(path); }
  bool rename(const char* pathFrom, const char* pathTo) override { return Storage.rename(pathFrom, pathTo); }
  bool remove(const char* path) override { return Storage.remove(path); }
  bool mkdir(const char* path) override { return Storage.mkdir(path); }
  bool rmdir(const char* path) override { return Storage.rmdir(path); }
};

}  // namespace

fs::FS& sdFatFS() {
  static fs::FS instance(std::make_shared<SdFatFSImpl>());
  return instance;
}
