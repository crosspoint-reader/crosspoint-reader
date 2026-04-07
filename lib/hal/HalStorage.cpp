#define HAL_STORAGE_IMPL
#include "HalStorage.h"

#include <FS.h>  // need to be included before SdFat.h for compatibility with FS.h's File class
#include <Logging.h>
#include <SDCardManager.h>
#include <SdFat.h>
#include <esp_task_wdt.h>

#include <cassert>

#define SDCard SDCardManager::getInstance()

HalStorage HalStorage::instance;

HalStorage::HalStorage() {
  storageMutex = xSemaphoreCreateMutex();
  assert(storageMutex != nullptr);
}

// begin() and ready() are only called from setup, no need to acquire mutex for them

bool HalStorage::begin() {
  const bool ok = SDCard.begin();
  if (ok) {
    // Pre-populate the total-bytes cache once (partition size never changes).
    sdTotalBytesCache = SDCard.cardTotalBytes();
    // Do an initial free-space walk synchronously — no other tasks are running yet during setup.
    sdFreeKiB = (uint32_t)(SDCard.cardFreeBytes() / 1024ULL);
    // Start the background refresh task.
    if (xTaskCreate(sdFreeUpdateTask, "sdFree", 2048, this, 1, &sdFreeUpdateTaskHandle) != pdPASS) {
      LOG_ERR("Storage", "Failed to create sdFree task; free-space cache will not update after writes");
      sdFreeUpdateTaskHandle = nullptr;
    }
  }
  return ok;
}

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
  bool ok;
  {
    StorageLock lock;
    ok = SDCard.writeFile(path, content);
  }
  if (ok) notifySdFreeUpdate();
  return ok;
}

bool HalStorage::ensureDirectoryExists(const char* path) { HAL_STORAGE_WRAPPED_CALL(ensureDirectoryExists, path); }

uint64_t HalStorage::sdTotalBytes() const {
  // Return 0 when the card is absent so callers see "no card" rather than a stale value.
  // SDCard.ready() is a simple bool read — safe without storageMutex.
  return SDCard.ready() ? sdTotalBytesCache : 0ULL;
}

uint64_t HalStorage::sdFreeBytes() const {
  // sdFreeKiB is a volatile uint32_t written atomically on single-core RISC-V — no mutex needed.
  // Return 0 when the card is absent.
  return SDCard.ready() ? (uint64_t)sdFreeKiB * 1024ULL : 0ULL;
}

uint64_t HalStorage::sdUsedBytes() const {
  const uint64_t total = sdTotalBytes();
  const uint64_t free = sdFreeBytes();
  return total > free ? total - free : 0;
}

void HalStorage::notifySdFreeUpdate() {
  if (sdFreeUpdateTaskHandle) {
    xTaskNotifyGive(sdFreeUpdateTaskHandle);
  } else {
    // Background task unavailable (creation failed); refresh synchronously.
    StorageLock lock;
    if (SDCard.ready()) {
      sdTotalBytesCache = SDCard.cardTotalBytes();
      const uint32_t freeKiB = (uint32_t)(SDCard.cardFreeBytes() / 1024ULL);
      if (freeKiB <= (uint32_t)(sdTotalBytesCache / 1024ULL)) sdFreeKiB = freeKiB;
    } else {
      sdTotalBytesCache = 0;
      sdFreeKiB = 0;
    }
  }
}

// Background task: wakes on notification, then waits for a 5-second quiet window before
// walking the FAT. This debounce ensures a batch of writes (e.g. 20 web uploads) triggers
// exactly one FAT walk — at the end of the batch, not during it.
void HalStorage::sdFreeUpdateTask(void* param) {
  auto& self = *static_cast<HalStorage*>(param);
  esp_task_wdt_add(NULL);  // register with watchdog so esp_task_wdt_reset() is effective
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // block until first notification
    while (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000))) {
    }  // reset window on each new notification
    esp_task_wdt_reset();  // FAT walk can be slow on large/fragmented cards
    {
      StorageLock lock;
      if (SDCard.ready()) {
        self.sdTotalBytesCache = SDCard.cardTotalBytes();
        const uint32_t freeKiB = (uint32_t)(SDCard.cardFreeBytes() / 1024ULL);
        if (freeKiB <= (uint32_t)(self.sdTotalBytesCache / 1024ULL)) self.sdFreeKiB = freeKiB;
      } else {
        self.sdTotalBytesCache = 0;
        self.sdFreeKiB = 0;
      }
    }
    esp_task_wdt_reset();  // reset again after the walk completes
  }
}

class HalFile::Impl {
 public:
  Impl(FsFile&& fsFile) : file(std::move(fsFile)) {}
  FsFile file;
};

HalFile::HalFile() = default;

HalFile::HalFile(std::unique_ptr<Impl> impl) : impl(std::move(impl)) {}

void HalFile::release() noexcept {
  if (!openedForWrite || !impl || !impl->file.isOpen()) return;
  bool needNotify = false;
  {
    HalStorage::StorageLock lock;
    if (impl->file.close()) {
      openedForWrite = false;
      needNotify = true;
    }
  }  // lock released before notifying, matching HalFile::close()
  if (needNotify) HalStorage::getInstance().notifySdFreeUpdate();
}

HalFile::~HalFile() { release(); }

// Move constructor: transfer ownership and clear openedForWrite on the moved-from object
// so a subsequent close() on it cannot trigger a spurious notify.
HalFile::HalFile(HalFile&& other) noexcept : impl(std::move(other.impl)), openedForWrite(other.openedForWrite) {
  other.openedForWrite = false;
}

HalFile& HalFile::operator=(HalFile&& other) noexcept {
  if (this != &other) {
    // release() closes any write-opened file on *this and notifies the free-space
    // cache before we overwrite impl. HalStorage::openFileForWrite always passes a
    // fresh HalFile (openedForWrite=false) so this branch is only reached in
    // user-level reassignment (no lock held).
    release();
    impl = std::move(other.impl);
    openedForWrite = other.openedForWrite;
    other.openedForWrite = false;
  }
  return *this;
}

HalFile HalStorage::open(const char* path, const oflag_t oflag) {
  StorageLock lock;  // ensure thread safety for the duration of this function
  return HalFile(std::make_unique<HalFile::Impl>(SDCard.open(path, oflag)));
}

bool HalStorage::mkdir(const char* path, const bool pFlag) { HAL_STORAGE_WRAPPED_CALL(mkdir, path, pFlag); }

bool HalStorage::exists(const char* path) { HAL_STORAGE_WRAPPED_CALL(exists, path); }

bool HalStorage::remove(const char* path) {
  bool ok;
  {
    StorageLock lock;
    ok = SDCard.remove(path);
  }
  if (ok) notifySdFreeUpdate();
  return ok;
}

bool HalStorage::rename(const char* oldPath, const char* newPath) {
  HAL_STORAGE_WRAPPED_CALL(rename, oldPath, newPath);
}

bool HalStorage::rmdir(const char* path) {
  bool ok;
  {
    StorageLock lock;
    ok = SDCard.rmdir(path);
  }
  if (ok) notifySdFreeUpdate();
  return ok;
}

bool HalStorage::openFileForRead(const char* moduleName, const char* path, HalFile& file) {
  StorageLock lock;  // ensure thread safety for the duration of this function
  // Assigning into a live write-opened HalFile would call operator= → release() → StorageLock,
  // re-entering the mutex we already hold and deadlocking.  Callers must pass a fresh handle.
  assert(!file.openedForWrite);
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

bool HalStorage::openFileForWrite(const char* moduleName, const char* path, HalFile& file, bool silent) {
  StorageLock lock;  // ensure thread safety for the duration of this function
  // Same invariant as openFileForRead: assigning into a live write-opened HalFile would
  // call operator= → release() → StorageLock, re-entering the held mutex and deadlocking.
  assert(!file.openedForWrite);
  FsFile fsFile;
  bool ok = SDCard.openFileForWrite(moduleName, path, fsFile);
  if (ok) {
    file = HalFile(std::make_unique<HalFile::Impl>(std::move(fsFile)));
    if (!silent) file.openedForWrite = true;
  }
  return ok;
}

bool HalStorage::openFileForWrite(const char* moduleName, const std::string& path, HalFile& file, bool silent) {
  return openFileForWrite(moduleName, path.c_str(), file, silent);
}

bool HalStorage::openFileForWrite(const char* moduleName, const String& path, HalFile& file, bool silent) {
  return openFileForWrite(moduleName, path.c_str(), file, silent);
}

bool HalStorage::removeDir(const char* path) {
  bool ok;
  {
    StorageLock lock;
    ok = SDCard.removeDir(path);
  }
  if (ok) notifySdFreeUpdate();
  return ok;
}

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
size_t HalFile::size() { HAL_FILE_FORWARD_CALL(size, ); }          // already thread-safe, no need to wrap
size_t HalFile::fileSize() { HAL_FILE_FORWARD_CALL(fileSize, ); }  // already thread-safe, no need to wrap
bool HalFile::seek(size_t pos) { HAL_FILE_WRAPPED_CALL(seekSet, pos); }
bool HalFile::seekCur(int64_t offset) { HAL_FILE_WRAPPED_CALL(seekCur, offset); }
bool HalFile::seekSet(size_t offset) { HAL_FILE_WRAPPED_CALL(seekSet, offset); }
int HalFile::available() const { HAL_FILE_WRAPPED_CALL(available, ); }
size_t HalFile::position() const { HAL_FILE_WRAPPED_CALL(position, ); }
int HalFile::read(void* buf, size_t count) { HAL_FILE_WRAPPED_CALL(read, buf, count); }
int HalFile::read() { HAL_FILE_WRAPPED_CALL(read, ); }
size_t HalFile::write(const void* buf, size_t count) { HAL_FILE_WRAPPED_CALL(write, buf, count); }
size_t HalFile::write(uint8_t b) { HAL_FILE_WRAPPED_CALL(write, b); }
bool HalFile::rename(const char* newPath) { HAL_FILE_WRAPPED_CALL(rename, newPath); }
bool HalFile::isDirectory() const { HAL_FILE_FORWARD_CALL(isDirectory, ); }  // already thread-safe, no need to wrap
void HalFile::rewindDirectory() { HAL_FILE_WRAPPED_CALL(rewindDirectory, ); }

bool HalFile::close() {
  bool needNotify = false;
  bool ok = false;
  {
    HalStorage::StorageLock lock;
    assert(impl != nullptr);
    ok = impl->file.close();
    if (ok && openedForWrite) {
      openedForWrite = false;  // clear before notify to prevent double-notify on double-close
      needNotify = true;
    }
  }  // lock released here before notifying
  if (needNotify) HalStorage::getInstance().notifySdFreeUpdate();
  return ok;
}

HalFile HalFile::openNextFile() {
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  return HalFile(std::make_unique<Impl>(impl->file.openNextFile()));
}
bool HalFile::isOpen() const { return impl != nullptr && impl->file.isOpen(); }  // already thread-safe, no need to wrap
HalFile::operator bool() const { return isOpen(); }
