#include "HalStorage.h"

#ifndef EMULATED
#include <SDCardManager.h>
#define SDCard SDCardManager::getInstance()
#else
#include <EmulationUtils.h>
#endif

HalStorage HalStorage::instance;

HalStorage::HalStorage() {}

bool HalStorage::begin() {
#ifndef EMULATED
  return SDCard.begin();
#else
  // wait for emulator server to be ready
  int64_t res = 0;
  while (res != 123456) {
    EmulationUtils::Lock lock;
    EmulationUtils::sendCmd(EmulationUtils::CMD_PING, "dummy");
    res = EmulationUtils::recvRespInt64(500);
  }

  return true;
#endif
}

bool HalStorage::ready() const {
#ifndef EMULATED
  return SDCard.ready();
#else
  // no-op
  return true;
#endif
}

std::vector<String> HalStorage::listFiles(const char* path, int maxFiles) {
#ifndef EMULATED
  return SDCard.listFiles(path, maxFiles);
#else
  Serial.printf("[%lu] [FS ] Emulated listFiles: %s\n", millis(), path);
  EmulationUtils::Lock lock;
  EmulationUtils::sendCmd(EmulationUtils::CMD_FS_LIST, path);
  std::vector<String> output;
  for (int i = 0; i < maxFiles; ++i) {
    auto resp = EmulationUtils::recvRespStr();
    if (resp.length() == 0) {
      break;
    }
    output.push_back(resp);
  }
  return output;
#endif
}

String HalStorage::readFile(const char* path) {
#ifndef EMULATED
  return SDCard.readFile(path);
#else
  Serial.printf("[%lu] [FS ] Emulated readFile: %s\n", millis(), path);
  EmulationUtils::Lock lock;
  EmulationUtils::sendCmd(EmulationUtils::CMD_FS_READ, path, "0", "-1");
  return EmulationUtils::recvRespStr();
#endif
}

#ifdef EMULATED
static int64_t getFileSizeEmulated(const char* path) {
  EmulationUtils::Lock lock;
  EmulationUtils::sendCmd(EmulationUtils::CMD_FS_STAT, path);
  return EmulationUtils::recvRespInt64();
}
#endif

bool HalStorage::readFileToStream(const char* path, Print& out, size_t chunkSize) {
#ifndef EMULATED
  return SDCard.readFileToStream(path, out, chunkSize);
#else
  Serial.printf("[%lu] [FS ] Emulated readFileToStream: %s\n", millis(), path);
  EmulationUtils::Lock lock;
  auto size = getFileSizeEmulated(path);
  if (size == -1) {
    Serial.printf("[%lu] [FS ] File not found: %s\n", millis(), path);
    return false;
  }
  if (size == -2) {
    Serial.printf("[%lu] [FS ] Path is a directory, not a file: %s\n", millis(), path);
    return false;
  }
  size_t bytesRead = 0;
  while (bytesRead < static_cast<size_t>(size)) {
    size_t toRead = std::min(chunkSize, static_cast<size_t>(size) - bytesRead);
    EmulationUtils::sendCmd(EmulationUtils::CMD_FS_READ, path, String(bytesRead).c_str(), String(toRead).c_str());
    auto buf = EmulationUtils::recvRespBuf();
    out.write(buf.data(), buf.size());
    bytesRead += buf.size();
  }
  return true;
#endif
}

size_t HalStorage::readFileToBuffer(const char* path, char* buffer, size_t bufferSize, size_t maxBytes) {
#ifndef EMULATED
  return SDCard.readFileToBuffer(path, buffer, bufferSize, maxBytes);
#else
  Serial.printf("[%lu] [FS ] Emulated readFileToBuffer: %s\n", millis(), path);
  EmulationUtils::Lock lock;
  auto size = getFileSizeEmulated(path);
  if (size == -1) {
    Serial.printf("[%lu] [FS ] File not found: %s\n", millis(), path);
    return 0;
  }
  if (size == -2) {
    Serial.printf("[%lu] [FS ] Path is a directory, not a file: %s\n", millis(), path);
    return 0;
  }
  size_t toRead = static_cast<size_t>(size);
  if (maxBytes > 0 && maxBytes < toRead) {
    toRead = maxBytes;
  }
  if (toRead >= bufferSize) {
    toRead = bufferSize - 1;  // leave space for null terminator
  }
  EmulationUtils::sendCmd(EmulationUtils::CMD_FS_READ, path, "0", String(toRead).c_str());
  auto buf = EmulationUtils::recvRespBuf();
  size_t bytesRead = buf.size();
  memcpy(buffer, buf.data(), bytesRead);
  buffer[bytesRead] = '\0';  // null-terminate
  return bytesRead;
#endif
}

bool HalStorage::writeFile(const char* path, const String& content) {
#ifndef EMULATED
  return SDCard.writeFile(path, content);
#else
  Serial.printf("[%lu] [FS ] Emulated writeFile: %s\n", millis(), path);
  EmulationUtils::Lock lock;
  std::string b64 = EmulationUtils::base64_encode((char*)content.c_str(), content.length());
  EmulationUtils::sendCmd(EmulationUtils::CMD_FS_WRITE, path, b64.c_str(), "0", "0");
  EmulationUtils::recvRespInt64();  // unused for now
  return true;
#endif
}

bool HalStorage::ensureDirectoryExists(const char* path) {
#ifndef EMULATED
  return SDCard.ensureDirectoryExists(path);
#else
  Serial.printf("[%lu] [FS ] Emulated ensureDirectoryExists: %s\n", millis(), path);
  EmulationUtils::Lock lock;
  EmulationUtils::sendCmd(EmulationUtils::CMD_FS_MKDIR, path);
  EmulationUtils::recvRespInt64();  // unused for now
  return true;
#endif
}

FsFile HalStorage::open(const char* path, const oflag_t oflag) {
#ifndef EMULATED
  return SDCard.open(path, oflag);
#else
  // TODO: do we need to check existence or create the file?
  return FsFile(path, oflag);
#endif
}

bool HalStorage::mkdir(const char* path, const bool pFlag) {
#ifndef EMULATED
  return SDCard.mkdir(path, pFlag);
#else
  Serial.printf("[%lu] [FS ] Emulated mkdir: %s\n", millis(), path);
  EmulationUtils::Lock lock;
  EmulationUtils::sendCmd(EmulationUtils::CMD_FS_MKDIR, path);
  EmulationUtils::recvRespInt64();  // unused for now
  return true;
#endif
}

bool HalStorage::exists(const char* path) {
#ifndef EMULATED
  return SDCard.exists(path);
#else
  Serial.printf("[%lu] [FS ] Emulated exists: %s\n", millis(), path);
  auto size = getFileSizeEmulated(path);
  return size != -1;
#endif
}

bool HalStorage::remove(const char* path) {
#ifndef EMULATED
  return SDCard.remove(path);
#else
  Serial.printf("[%lu] [FS ] Emulated remove: %s\n", millis(), path);
  EmulationUtils::Lock lock;
  EmulationUtils::sendCmd(EmulationUtils::CMD_FS_RM, path);
  EmulationUtils::recvRespInt64();  // unused for now
  return true;
#endif
}

bool HalStorage::rmdir(const char* path) {
#ifndef EMULATED
  return SDCard.rmdir(path);
#else
  Serial.printf("[%lu] [FS ] Emulated rmdir: %s\n", millis(), path);
  EmulationUtils::Lock lock;
  EmulationUtils::sendCmd(EmulationUtils::CMD_FS_RM, path);
  EmulationUtils::recvRespInt64();  // unused for now
  return true;
#endif
}

bool HalStorage::openFileForRead(const char* moduleName, const char* path, FsFile& file) {
#ifndef EMULATED
  return SDCard.openFileForRead(moduleName, path, file);
#else
  Serial.printf("[%lu] [FS ] Emulated openFileForRead: %s\n", millis(), path);
  auto size = getFileSizeEmulated(path);
  if (size == -1) {
    Serial.printf("[%lu] [FS ] File not found: %s\n", millis(), path);
    return false;
  }
  if (size == -2) {
    Serial.printf("[%lu] [FS ] Path is a directory, not a file: %s\n", millis(), path);
    return false;
  }
  file = FsFile(path, O_RDONLY);
  return true;
#endif
}

bool HalStorage::openFileForRead(const char* moduleName, const std::string& path, FsFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForRead(const char* moduleName, const String& path, FsFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const char* path, FsFile& file) {
#ifndef EMULATED
  return SDCard.openFileForWrite(moduleName, path, file);
#else
  Serial.printf("[%lu] [FS ] Emulated openFileForWrite: %s\n", millis(), path);
  auto size = getFileSizeEmulated(path);
  if (size == -1) {
    Serial.printf("[%lu] [FS ] File does not exist and will be created\n", millis());
  } else if (size == -2) {
    Serial.printf("[%lu] [FS ] Path is a directory, not a file: %s\n", millis(), path);
    return false;
  }
  file = FsFile(path, O_WRONLY | O_CREAT);
  return true;
#endif
}

bool HalStorage::openFileForWrite(const char* moduleName, const std::string& path, FsFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const String& path, FsFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::removeDir(const char* path) {
#ifndef EMULATED
  return SDCard.removeDir(path);
#else
  // TODO: implement this
  return false;
#endif
}

#ifdef EMULATED
//
// FsFile emulation methods
//

String nameFromPath(const String& path) {
  int lastSlash = path.lastIndexOf('/');
  if (lastSlash == -1) {
    return path;
  } else {
    return path.substring(lastSlash + 1);
  }
}

FsFile::FsFile(const char* path, oflag_t oflag) : path(path), oflag(oflag) {
  Serial.printf("[%lu] [FSF] Emulated FsFile open: %s\n", millis(), path);
  auto size = getFileSizeEmulated(path);
  if (size == -1) {
    if (oflag & O_CREAT) {
      Serial.printf("[%lu] [FSF] File does not exist, will be created: %s\n", millis(), path);
      open = true;
      fileSizeBytes = 0;
      write((const uint8_t*)"", 0);  // create the file
    } else {
      Serial.printf("[%lu] [FSF] File not found: %s\n", millis(), path);
      open = false;
    }
  } else if (size == -2) {
    Serial.printf("[%lu] [FSF] Path is a directory: %s\n", millis(), path);
    open = true;
    isDir = true;
    name = nameFromPath(String(path));
    // get directory entries
    EmulationUtils::Lock lock;
    EmulationUtils::sendCmd(EmulationUtils::CMD_FS_LIST, path);
    dirEntries.clear();
    while (true) {
      auto resp = EmulationUtils::recvRespStr();
      if (resp.length() == 0) {
        break;
      }
      dirEntries.push_back(resp);
    }
    Serial.printf("[%lu] [FSF] Directory has %u entries\n", millis(), (unsigned)dirEntries.size());
    dirIndex = 0;
    fileSizeBytes = 0;
  } else {
    Serial.printf("[%lu] [FSF] File opened, size: %lld bytes: %s\n", millis(), size, path);
    open = true;
    fileSizeBytes = static_cast<size_t>(size);
    name = nameFromPath(String(path));
  }
}

int FsFile::read(void* buf, size_t count) {
  if (!open || isDir) return -1;
  size_t bytesAvailable = (fileSizeBytes > filePos) ? (fileSizeBytes - filePos) : 0;
  if (bytesAvailable == 0) return 0;
  size_t toRead = std::min(count, bytesAvailable);
  EmulationUtils::Lock lock;
  EmulationUtils::sendCmd(EmulationUtils::CMD_FS_READ, path.c_str(), String(filePos).c_str(), String(toRead).c_str());
  auto data = EmulationUtils::recvRespBuf();
  size_t bytesRead = data.size();
  memcpy(buf, data.data(), bytesRead);
  filePos += bytesRead;
  return static_cast<int>(bytesRead);
}

int FsFile::read() {
  uint8_t b;
  int result = read(&b, 1);
  if (result <= 0) return -1;
  return b;
}

size_t FsFile::write(const uint8_t* buffer, size_t size) {
  if (!open || isDir) return 0;
  Serial.printf("[%lu] [FSF] Emulated FsFile write: %s (size: %u)\n", millis(), path.c_str(), (unsigned)size);
  std::string b64 = EmulationUtils::base64_encode((const char*)buffer, size);
  EmulationUtils::Lock lock;
  EmulationUtils::sendCmd(EmulationUtils::CMD_FS_WRITE, path.c_str(), b64.c_str(), String(filePos).c_str(), "1");
  EmulationUtils::recvRespInt64();  // unused for now
  filePos += size;
  if (filePos > fileSizeBytes) {
    fileSizeBytes = filePos;
  }
  return size;
}

size_t FsFile::write(uint8_t b) { return write(&b, 1); }

bool FsFile::rename(const char* newPath) {
  if (!open) return false;
  Serial.printf("[%lu] [FSF] Emulated FsFile rename: %s -> %s\n", millis(), path.c_str(), newPath);
  EmulationUtils::Lock lock;
  EmulationUtils::sendCmd(EmulationUtils::CMD_FS_MOVE, path.c_str(), newPath);
  auto res = EmulationUtils::recvRespInt64();
  if (res != 0) {
    Serial.printf("[%lu] [FSF] Failed to rename file: %s -> %s\n", millis(), path.c_str(), newPath);
    return false;
  }
  path = String(newPath);
  name = nameFromPath(path);
  return true;
}

#endif