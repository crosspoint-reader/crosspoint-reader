#pragma once
// Real-filesystem shim for host tests that exercise ZipFile or CssParser.
// Implements FsFile on top of stdio so tests can read actual .epub files
// and write real cache files under /tmp.

#include <fcntl.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

// Device HalStorage.h transitively provides the Arduino core (millis, ESP);
// mirror that so TUs which only include HalStorage.h still compile.
#include "Arduino.h"
#include "Print.h"
#include "WString.h"

class HalFile : public Print {
 public:
  HalFile() = default;
  ~HalFile() { closeQuiet(); }

  HalFile(HalFile&& o) noexcept
      : fp_(o.fp_),
        hasImpl_(o.hasImpl_),
        isDir_(o.isDir_),
        hasName_(o.hasName_),
        name_(std::move(o.name_)),
        entrySize_(o.entrySize_),
        entryDate_(o.entryDate_),
        entryTime_(o.entryTime_),
        entries_(std::move(o.entries_)),
        next_(o.next_) {
    o.fp_ = nullptr;
    o.hasImpl_ = false;
    o.isDir_ = false;
    o.hasName_ = false;
    o.next_ = 0;
  }
  HalFile& operator=(HalFile&& o) noexcept {
    if (this != &o) {
      closeQuiet();
      fp_ = o.fp_;
      hasImpl_ = o.hasImpl_;
      isDir_ = o.isDir_;
      hasName_ = o.hasName_;
      name_ = std::move(o.name_);
      entrySize_ = o.entrySize_;
      entryDate_ = o.entryDate_;
      entryTime_ = o.entryTime_;
      entries_ = std::move(o.entries_);
      next_ = o.next_;
      o.fp_ = nullptr;
      o.hasImpl_ = false;
      o.isDir_ = false;
      o.hasName_ = false;
      o.next_ = 0;
    }
    return *this;
  }
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  // WH_HOST_STDIO_UNBUFFERED=1 drops glibc's lazily malloc'd 4 KB buffer (one per open FILE*),
  // which has no device counterpart: SdFat reads through the card driver's shared sector cache.
  // The epub_pipeline heap census sets it so those buffers do not appear as allocation sites
  // (they showed up as 4 KB x 600 under a misleading symbol). Every read is then a syscall, so
  // it stays off by default.
  static void applyStdioPolicy(FILE* fp) {
    static const bool unbuffered = std::getenv("WH_HOST_STDIO_UNBUFFERED") != nullptr;
    if (fp != nullptr && unbuffered) setvbuf(fp, nullptr, _IONBF, 0);
  }

  bool openForRead(const std::string& path) {
    closeQuiet();
    hasImpl_ = true;
    fp_ = fopen(path.c_str(), "rb");
    applyStdioPolicy(fp_);
    return fp_ != nullptr;
  }

  bool openForWrite(const std::string& path) {
    closeQuiet();
    hasImpl_ = true;
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
      std::error_code ec;
      std::filesystem::create_directories(parent, ec);
      if (ec) return false;
    }
    // Read/write, as on the device (SDCardManager::openFileForWrite: O_RDWR | O_CREAT | O_TRUNC):
    // a section build reads its anchor map back through the write handle.
    fp_ = fopen(path.c_str(), "w+b");
    applyStdioPolicy(fp_);
    return fp_ != nullptr;
  }

  // Mirrors HalStorage::openFileForUpdate: existing file, read+write, no truncation.
  bool openForUpdate(const std::string& path) {
    closeQuiet();
    hasImpl_ = true;
    fp_ = fopen(path.c_str(), "r+b");
    applyStdioPolicy(fp_);
    return fp_ != nullptr;
  }

  // Device parity, deliberately fatal. HalStorage gives every handle it opens an Impl — even when
  // the open FAILED — so on device close() is fine on an opened-and-failed handle but asserts on
  // one that was never opened at all (or was moved from). stdio cannot tell those apart, so
  // hasImpl_ models it: without this, closing a never-opened handle is silently fine on the host
  // and aborts in the reader's hands. Not assert(): host tests build with NDEBUG.
  bool close() {
    if (!hasImpl_) {
      std::fprintf(stderr, "HalFile::close() on a handle that was never opened — this aborts on device\n");
      std::abort();
    }
    return closeQuiet();
  }

  // Directory handle, so tests can exercise code that walks the SD card
  // (DictionaryRegistry scans /dictionaries/*/ for index stems). Entries are
  // snapshotted and sorted at open time: std::filesystem iteration order is
  // unspecified, and device SdFat order is not reproducible either, so a test
  // that depends on it would be flaky rather than wrong.
  bool openDir(const std::string& path) {
    closeQuiet();
    hasImpl_ = true;
    std::error_code ec;
    if (!std::filesystem::is_directory(path, ec) || ec) return false;
    isDir_ = true;
    for (const auto& e : std::filesystem::directory_iterator(path, ec)) {
      DirEntry entry{e.path().filename().string(), e.is_directory()};
      if (!entry.isDir) {
        std::error_code sizeError;
        entry.size = static_cast<uint64_t>(std::filesystem::file_size(e.path(), sizeError));
      }
      fatStamp(e.path(), entry.fatDate, entry.fatTime);  // a folder's entry carries a date too
      entries_.push_back(std::move(entry));
    }
    std::sort(entries_.begin(), entries_.end(), [](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });
    return true;
  }

  bool isOpen() const { return fp_ != nullptr; }
  explicit operator bool() const { return fp_ != nullptr || isDir_ || hasName_; }

  // Test fault injection: a read that starts at or past this offset fails, as a flaky card read
  // would. -1 (the default) turns it off.
  static inline long failReadsFrom = -1;
  // The same for one read only: the first read starting at or past this offset fails, and the
  // hook turns itself off -- a single bad sector, with everything after it readable again.
  static inline long failOneReadFrom = -1;
  // Every read that starts exactly at this offset fails, reads elsewhere do not: hits one known
  // record (a ZIP entry's data) even when the reader visits later offsets first (the central
  // directory at the end of the archive). -1 turns it off.
  static inline long failReadAt = -1;
  // Test instrumentation: bytes delivered by read(), across all files. Lets a test bound how much
  // a pass reads, where wall-clock time on a host says little about SD cost.
  static inline size_t bytesRead = 0;

  int read(void* buf, size_t n) {
    if (!fp_) return -1;
    if (failReadsFrom >= 0 && ftell(fp_) >= failReadsFrom) return -1;
    if (failReadAt >= 0 && ftell(fp_) == failReadAt) return -1;
    if (failOneReadFrom >= 0 && ftell(fp_) >= failOneReadFrom) {
      failOneReadFrom = -1;
      return -1;
    }
    const size_t got = fread(buf, 1, n, fp_);
    bytesRead += got;
    return static_cast<int>(got);
  }
  // Single-byte read, SdFat-style: returns the byte or -1 (used by Bitmap.cpp).
  int read() {
    if (!fp_) return -1;
    return fgetc(fp_);
  }

  // A folder's position is where its listing stands, counted in 32-byte directory entries as on
  // FAT (one per name here; FAT spends more on a long one), so a walk can close a folder and seek
  // back into its listing (FolderWalk).
  static constexpr size_t kDirEntryBytes = 32;
  bool seek(size_t pos) {
    if (isDir_) {
      if (pos % kDirEntryBytes != 0 || pos / kDirEntryBytes > entries_.size()) return false;
      next_ = pos / kDirEntryBytes;
      return true;
    }
    return fp_ && fseek(fp_, static_cast<long>(pos), SEEK_SET) == 0;
  }
  bool seekSet(size_t pos) { return seek(pos); }
  bool seekCur(int64_t offset) { return fp_ && fseek(fp_, static_cast<long>(offset), SEEK_CUR) == 0; }
  bool seek64(uint64_t pos) { return fp_ && fseek(fp_, static_cast<long>(pos), SEEK_SET) == 0; }
  bool seekSet64(uint64_t pos) { return seek64(pos); }

  size_t position() const {
    if (isDir_) return next_ * kDirEntryBytes;
    return fp_ ? static_cast<size_t>(ftell(fp_)) : 0;
  }
  uint64_t position64() const { return position(); }
  // Bytes left to read, as on device: EpubImageManifest::load() sizes its corruption guard from
  // this, so a constant here made every persisted manifest look corrupt on the host.
  int available() const {
    if (!fp_) return 0;
    const long cur = ftell(fp_);
    fseek(fp_, 0, SEEK_END);
    const long end = ftell(fp_);
    fseek(fp_, cur, SEEK_SET);
    return end > cur ? static_cast<int>(end - cur) : 0;
  }

  size_t size() { return fileSize(); }
  size_t fileSize() {
    if (!fp_) return hasName_ ? static_cast<size_t>(entrySize_) : 0;
    const long cur = ftell(fp_);
    fseek(fp_, 0, SEEK_END);
    const long sz = ftell(fp_);
    fseek(fp_, cur, SEEK_SET);
    return static_cast<size_t>(sz);
  }
  uint64_t size64() { return fileSize(); }
  uint64_t fileSize64() { return fileSize(); }

  size_t write(const uint8_t* data, size_t size) {
    if (!fp_) return 0;
    return fwrite(data, 1, size, fp_);
  }
  // Device SdFat accepts void*; Page.cpp writes char arrays through this.
  size_t write(const void* data, size_t size) {
    if (!fp_) return 0;
    return fwrite(data, 1, size, fp_);
  }
  size_t write(uint8_t c) override {
    if (!fp_) return 0;
    return fwrite(&c, 1, 1, fp_);
  }
  void flush() {
    if (fp_) fflush(fp_);
  }
  size_t getName(char* out, size_t len) {
    if (!out || len == 0) return 0;
    const size_t n = std::min(name_.size(), len - 1);
    std::memcpy(out, name_.data(), n);
    out[n] = '\0';
    return n;
  }
  bool rename(const char*) { return false; }
  bool getModifyDateTime(uint16_t* pdate, uint16_t* ptime) {
    if (!hasName_ || pdate == nullptr || ptime == nullptr) return false;
    *pdate = entryDate_;
    *ptime = entryTime_;
    return true;
  }
  // std::filesystem keeps no creation time: the write time stands in for it.
  bool getCreateDateTime(uint16_t* pdate, uint16_t* ptime) { return getModifyDateTime(pdate, ptime); }
  bool isDirectory() const { return isDir_; }
  void rewindDirectory() { next_ = 0; }
  // One entry per call, exhausted-directory returns a falsy handle. Only the
  // name and the directory flag are populated — that is all a scan reads.
  HalFile openNextFile() {
    HalFile f;
    if (next_ >= entries_.size()) return f;
    const DirEntry& e = entries_[next_++];
    f.hasImpl_ = true;
    f.hasName_ = true;
    f.name_ = e.name;
    f.isDir_ = e.isDir;
    f.entrySize_ = e.size;
    f.entryDate_ = e.fatDate;
    f.entryTime_ = e.fatTime;
    return f;
  }

 private:
  // A file's last write as FAT date and time, in UTC: the stamp SdFat reads from a directory entry.
  static void fatStamp(const std::filesystem::path& path, uint16_t& date, uint16_t& time) {
    std::error_code ec;
    const auto written = std::filesystem::last_write_time(path, ec);
    date = 0;
    time = 0;
    if (ec) return;
    const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::file_clock::to_sys(written));
    const std::time_t t = std::chrono::system_clock::to_time_t(seconds);
    const std::tm* tm = std::gmtime(&t);
    if (tm == nullptr || tm->tm_year < 80) return;
    date = static_cast<uint16_t>(((tm->tm_year - 80) << 9) | ((tm->tm_mon + 1) << 5) | tm->tm_mday);
    time = static_cast<uint16_t>((tm->tm_hour << 11) | (tm->tm_min << 5) | (tm->tm_sec / 2));
  }

  // close() without the device-parity check: for the destructor and the open helpers, which must
  // both tolerate a handle that was never opened.
  bool closeQuiet() {
    if (fp_) {
      fclose(fp_);
      fp_ = nullptr;
    }
    isDir_ = false;
    entries_.clear();
    next_ = 0;
    return true;
  }

  struct DirEntry {
    std::string name;
    bool isDir = false;
    uint64_t size = 0;
    uint16_t fatDate = 0;
    uint16_t fatTime = 0;
  };

  FILE* fp_ = nullptr;
  bool hasImpl_ = false;
  bool isDir_ = false;
  bool hasName_ = false;
  std::string name_;
  uint64_t entrySize_ = 0;  // a directory entry's size and FAT stamp, as SdFat's openNext gives them
  uint16_t entryDate_ = 0;
  uint16_t entryTime_ = 0;
  std::vector<DirEntry> entries_;
  size_t next_ = 0;
};

using FsFile = HalFile;

class HalStorage {
 public:
  bool openFileForRead(const char*, const std::string& path, HalFile& f) { return f.openForRead(path); }
  bool openFileForRead(const char*, const char* path, HalFile& f) { return f.openForRead(path); }
  bool openFileForWrite(const char*, const std::string& path, HalFile& f) { return f.openForWrite(path); }
  bool openFileForWrite(const char*, const char* path, HalFile& f) { return f.openForWrite(path); }
  bool openFileForUpdate(const char*, const std::string& path, HalFile& f) { return f.openForUpdate(path); }
  bool openFileForUpdate(const char*, const char* path, HalFile& f) { return f.openForUpdate(path); }
  bool exists(const char* path) { return std::filesystem::exists(path); }
  // The device counts the firmware's own writes, for caches keyed on the card's contents; a host
  // test changes its tree between runs, not under a cache.
  uint32_t contentGeneration() const { return 0; }
  // Mirrors the device HalStorage: stream a whole file into a Print sink in
  // chunks. Epub::applyMetadataSidecar() feeds ContentOpfParser through this,
  // so the host pipeline exercises the same code path the device runs.
  bool readFileToStream(const char* path, Print& out, size_t chunkSize = 256) {
    HalFile f;
    if (!f.openForRead(path)) return false;
    std::vector<uint8_t> buf(chunkSize ? chunkSize : 256);
    for (;;) {
      const int n = f.read(buf.data(), buf.size());
      if (n < 0) return false;
      if (n == 0) break;
      if (out.write(buf.data(), static_cast<size_t>(n)) != static_cast<size_t>(n)) return false;
    }
    return true;
  }
  // Mirrors the device HalStorage: the whole file, its size checked against `cap` first. Fails on a
  // missing, directory, empty, above-`cap` or short-read file.
  bool readFileToString(const char*, const std::string& path, size_t cap, std::string& out) {
    out.clear();
    HalFile f;
    if (!f.openForRead(path) || f.isDirectory()) return false;
    const size_t size = f.fileSize();
    if (size == 0 || size > cap) return false;
    out.resize(size);
    return f.read(out.data(), size) == static_cast<int>(size);
  }
  bool mkdir(const char* path, bool recursive = true) {
    std::error_code ec;
    return recursive ? std::filesystem::create_directories(path, ec) : std::filesystem::create_directory(path, ec);
  }
  bool remove(const char* path) { return std::filesystem::remove(path); }
  bool rename(const char* oldPath, const char* newPath) {
    std::error_code ec;
    std::filesystem::rename(oldPath, newPath, ec);
    return !ec;
  }
  bool removeDir(const char* path) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    return !ec;
  }
  using oflag_t = int;
  HalFile open(const char* path, oflag_t = O_RDONLY) {
    HalFile f;
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec) && !ec) {
      f.openDir(path);
    } else {
      f.openForRead(path);
    }
    return f;
  }
  // Sorted for determinism: std::filesystem iteration order is unspecified and
  // the pipeline harness requires byte-identical behavior across runs.
  std::vector<String> listFiles(const char* path = "/", int maxFiles = 200) {
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(path, ec)) {
      if (e.is_regular_file()) names.push_back(e.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    if (names.size() > static_cast<size_t>(maxFiles)) names.resize(maxFiles);
    std::vector<String> out;
    out.reserve(names.size());
    for (auto& n : names) out.push_back(String(std::move(n)));
    return out;
  }
  static HalStorage& getInstance() {
    static HalStorage i;
    return i;
  }
};

#define Storage HalStorage::getInstance()
