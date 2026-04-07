#pragma once

#include <Print.h>
#include <common/FsApiConstants.h>  // for oflag_t
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <memory>
#include <string>
#include <vector>

class HalFile;

class HalStorage {
 public:
  HalStorage();
  bool begin();
  bool ready() const;
  std::vector<String> listFiles(const char* path = "/", int maxFiles = 200);
  // Read the entire file at `path` into a String. Returns empty string on failure.
  String readFile(const char* path);
  // Low-memory helpers:
  // Stream the file contents to a `Print` (e.g. `Serial`, or any `Print`-derived object).
  // Returns true on success, false on failure.
  bool readFileToStream(const char* path, Print& out, size_t chunkSize = 256);
  // Read up to `bufferSize-1` bytes into `buffer`, null-terminating it. Returns bytes read.
  size_t readFileToBuffer(const char* path, char* buffer, size_t bufferSize, size_t maxBytes = 0);
  // Write a string to `path` on the SD card. Overwrites existing file.
  // Returns true on success.
  bool writeFile(const char* path, const String& content);
  // Ensure a directory exists, creating it if necessary. Returns true on success.
  bool ensureDirectoryExists(const char* path);

  HalFile open(const char* path, const oflag_t oflag = O_RDONLY);
  bool mkdir(const char* path, const bool pFlag = true);
  bool exists(const char* path);
  bool remove(const char* path);
  bool rename(const char* oldPath, const char* newPath);
  bool rmdir(const char* path);

  bool openFileForRead(const char* moduleName, const char* path, HalFile& file);
  bool openFileForRead(const char* moduleName, const std::string& path, HalFile& file);
  bool openFileForRead(const char* moduleName, const String& path, HalFile& file);
  // silent=true skips the free-space cache notification on close (use for frequent
  // small writes like progress saves that do not meaningfully change free space).
  bool openFileForWrite(const char* moduleName, const char* path, HalFile& file, bool silent = false);
  bool openFileForWrite(const char* moduleName, const std::string& path, HalFile& file, bool silent = false);
  bool openFileForWrite(const char* moduleName, const String& path, HalFile& file, bool silent = false);
  bool removeDir(const char* path);

  // Returns total SD card size in bytes (cached — fast, no SD access).
  uint64_t sdTotalBytes() const;
  // Returns used space in bytes (total minus free, both cached — fast).
  // NOTE: quantised to 1 KiB increments — the internal cache stores kibibytes.
  uint64_t sdUsedBytes() const;
  // Returns free space in bytes (cached — fast, no SD access).
  // NOTE: quantised to 1 KiB increments — the internal cache stores kibibytes.
  uint64_t sdFreeBytes() const;

  static HalStorage& getInstance() { return instance; }

  class StorageLock;     // private class, used internally
  friend class HalFile;  // HalFile::close() calls notifySdFreeUpdate()

 private:
  static HalStorage instance;

  bool initialized = false;
  SemaphoreHandle_t storageMutex = nullptr;

  // Free-space cache.
  // sdTotalBytesCache is refreshed by notifySdFreeUpdate/sdFreeUpdateTask so that a
  // card swap at runtime updates the displayed capacity.  It is always written under
  // storageMutex and read lock-free; on single-core RISC-V two-word writes are never
  // split by a context switch, so no torn reads occur in practice.
  // sdFreeKiB stores free space in 1 KiB units (cardFreeBytes / 1024), giving ~1 KiB
  // quantisation while keeping the field a uint32_t (atomic on 32-bit RISC-V, no mutex needed).
  // Supports SD cards up to ~4 TB (2^32 * 1024).
  // Both caches are set to 0 when the card is not ready (removed/unmounted).
  uint64_t sdTotalBytesCache = 0;
  volatile uint32_t sdFreeKiB = 0;

  // Background task: blocks until notified, then waits for a 5-second quiet window
  // (debounce) before walking the FAT to refresh sdFreeMB. This ensures that even a
  // large batch of uploads or deletes triggers exactly one FAT walk, at the end.
  static void sdFreeUpdateTask(void* param);
  TaskHandle_t sdFreeUpdateTaskHandle = nullptr;

  // Wake the background task. Safe to call from any task; no-op if the task was not started.
  void notifySdFreeUpdate();
};

#define Storage HalStorage::getInstance()

class HalFile : public Print {
  friend class HalStorage;
  class Impl;
  std::unique_ptr<Impl> impl;
  // Set by HalStorage::openFileForWrite; cleared after notifying sdFreeUpdateTask in close().
  // Prevents read-only file closes from triggering unnecessary FAT walks.
  bool openedForWrite = false;
  explicit HalFile(std::unique_ptr<Impl> impl);
  // Close the underlying file and notify the free-space cache if this instance
  // was opened for writing. Idempotent; safe to call when impl is null or file
  // is already closed. Used by operator= and ~HalFile to preserve notify
  // semantics without duplicating the logic.
  void release() noexcept;

 public:
  HalFile();
  ~HalFile();
  HalFile(HalFile&&) noexcept;
  HalFile& operator=(HalFile&&) noexcept;
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  void flush();
  size_t getName(char* name, size_t len);
  size_t size();
  size_t fileSize();
  bool seek(size_t pos);
  bool seekCur(int64_t offset);
  bool seekSet(size_t offset);
  int available() const;
  size_t position() const;
  int read(void* buf, size_t count);
  int read();  // read a single byte
  size_t write(const void* buf, size_t count);
  size_t write(uint8_t b) override;
  bool rename(const char* newPath);
  bool isDirectory() const;
  void rewindDirectory();
  bool close();
  HalFile openNextFile();
  bool isOpen() const;
  operator bool() const;
};

// Only do renaming FsFile to HalFile if this header is included by downstream code
// The renaming is to allow using the thread-safe HalFile instead of the raw FsFile, without needing to change the
// downstream code
#ifndef HAL_STORAGE_IMPL
using FsFile = HalFile;
#endif

// Downstream code must use Storage instead of SdMan
#ifdef SdMan
#undef SdMan
#endif
