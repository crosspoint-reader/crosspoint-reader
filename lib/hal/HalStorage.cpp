#include "HalStorage.h"
#include "EmulationUtils.h"
#include <vector>

#if CROSSPOINT_EMULATED == 0
#include <SDCardManager.h>
#endif

HalStorage HalStorage::instance;

HalStorage::HalStorage() {}

bool HalStorage::begin() {
#if CROSSPOINT_EMULATED == 0
  return SdMan.begin();
#else
  // no-op
  return true;
#endif
}

bool HalStorage::ready() const {
#if CROSSPOINT_EMULATED == 0
  return SdMan.ready();
#else
  // no-op
  return true;
#endif
}

std::vector<String> HalStorage::listFiles(const char* path, int maxFiles) {
#if CROSSPOINT_EMULATED == 0
  return SdMan.listFiles(path, maxFiles);
#else
  Serial.printf("[%lu] [FS ] Emulated listFiles: %s\n", millis(), path);
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
#if CROSSPOINT_EMULATED == 0
  return SdMan.readFile(path);
#else
  Serial.printf("[%lu] [FS ] Emulated readFile: %s\n", millis(), path);
  EmulationUtils::sendCmd(EmulationUtils::CMD_FS_READ, path, "0", "-1");
  return EmulationUtils::recvRespStr();
#endif
}

static int64_t getFileSizeEmulated(const char* path) {
  EmulationUtils::sendCmd(EmulationUtils::CMD_FS_STAT, path);
  return EmulationUtils::recvRespInt64();
}

bool HalStorage::readFileToStream(const char* path, Print& out, size_t chunkSize) {
#if CROSSPOINT_EMULATED == 0
  return SdMan.readFileToStream(path, out, chunkSize);
#else
  Serial.printf("[%lu] [FS ] Emulated readFileToStream: %s\n", millis(), path);
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
#if CROSSPOINT_EMULATED == 0
  return SdMan.readFileToBuffer(path, buffer, bufferSize, maxBytes);
#else
  Serial.printf("[%lu] [FS ] Emulated readFileToBuffer: %s\n", millis(), path);
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
    toRead = bufferSize - 1; // leave space for null terminator
  }
  EmulationUtils::sendCmd(EmulationUtils::CMD_FS_READ, path, "0", String(toRead).c_str());
  auto buf = EmulationUtils::recvRespBuf();
  size_t bytesRead = buf.size();
  memcpy(buffer, buf.data(), bytesRead);
  buffer[bytesRead] = '\0'; // null-terminate
  return bytesRead;
#endif
}

bool HalStorage::writeFile(const char* path, const String& content) {
#if CROSSPOINT_EMULATED == 0
  return SdMan.writeFile(path, content);
#else
  Serial.printf("[%lu] [FS ] Emulated writeFile: %s\n", millis(), path);
  std::string b64 = EmulationUtils::base64_encode((char*)content.c_str(), content.length());
  EmulationUtils::sendCmd(EmulationUtils::CMD_FS_WRITE, path, b64.c_str(), "0", "0");
  EmulationUtils::recvRespInt64(); // unused for now
  return true;
#endif
}

bool HalStorage::ensureDirectoryExists(const char* path) {
#if CROSSPOINT_EMULATED == 0
  return SdMan.ensureDirectoryExists(path);
#else
  Serial.printf("[%lu] [FS ] Emulated ensureDirectoryExists: %s\n", millis(), path);
  EmulationUtils::sendCmd(EmulationUtils::CMD_FS_MKDIR, path);
  EmulationUtils::recvRespInt64(); // unused for now
  return true;
#endif
}

FsFile HalStorage::open(const char* path, const oflag_t oflag) {
#if CROSSPOINT_EMULATED == 0
  return SdMan.open(path, oflag);
#else
  // TODO: do we need to check existence or create the file?
  return FsFile(path, oflag);
#endif
}

bool HalStorage::mkdir(const char* path, const bool pFlag) {
#if CROSSPOINT_EMULATED == 0
  return SdMan.mkdir(path, pFlag);
#else
  EmulationUtils::sendCmd(EmulationUtils::CMD_FS_MKDIR, path);
  EmulationUtils::recvRespInt64(); // unused for now
  return true;
#endif
}

bool HalStorage::exists(const char* path) {
#if CROSSPOINT_EMULATED == 0
  return SdMan.exists(path);
#else
  Serial.printf("[%lu] [FS ] Emulated exists: %s\n", millis(), path);
  auto size = getFileSizeEmulated(path);
  return size != -1;
#endif
}

bool HalStorage::remove(const char* path) {
#if CROSSPOINT_EMULATED == 0
  return SdMan.remove(path);
#else
  Serial.printf("[%lu] [FS ] Emulated remove: %s\n", millis(), path);
  EmulationUtils::sendCmd(EmulationUtils::CMD_FS_RM, path);
  EmulationUtils::recvRespInt64(); // unused for now
  return true;
#endif
}

bool HalStorage::rmdir(const char* path) {
#if CROSSPOINT_EMULATED == 0
  return SdMan.rmdir(path);
#else
  Serial.printf("[%lu] [FS ] Emulated rmdir: %s\n", millis(), path);
  EmulationUtils::sendCmd(EmulationUtils::CMD_FS_RM, path);
  EmulationUtils::recvRespInt64(); // unused for now
  return true;
#endif
}

bool HalStorage::openFileForRead(const char* moduleName, const char* path, FsFile& file) {
#if CROSSPOINT_EMULATED == 0
  return SdMan.openFileForRead(moduleName, path, file);
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
#if CROSSPOINT_EMULATED == 0
  return SdMan.openFileForWrite(moduleName, path, file);
#else
  Serial.printf("[%lu] [FS ] Emulated openFileForWrite: %s\n", millis(), path);
  auto size = getFileSizeEmulated(path);
  if (size == -1) {
    Serial.printf("[%lu] [FS ] File not found: %s\n", millis(), path);
    return false;
  }
  if (size == -2) {
    Serial.printf("[%lu] [FS ] Path is a directory, not a file: %s\n", millis(), path);
    return false;
  }
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
#if CROSSPOINT_EMULATED == 0
  return SdMan.removeDir(path);
#else
  // to be implemented
  return false;
#endif
}



#if CROSSPOINT_EMULATED == 1
//
// FsFile emulation methods
//

FsFile::FsFile(const char* path, oflag_t oflag) : path(path), oflag(oflag) {
  Serial.printf("[%lu] [FS ] Emulated FsFile open: %s\n", millis(), path);
  auto size = getFileSizeEmulated(path);
  if (size == -1) {
    Serial.printf("[%lu] [FS ] File not found: %s\n", millis(), path);
    open = false;
    fileSizeBytes = 0;
  } else if (isDir) {
    Serial.printf("[%lu] [FS ] Path is a directory: %s\n", millis(), path);
    open = true;
    // get directory entries
    EmulationUtils::sendCmd(EmulationUtils::CMD_FS_LIST, path);
    dirEntries.clear();
    while (true) {
      auto resp = EmulationUtils::recvRespStr();
      if (resp.length() == 0) {
        break;
      }
      dirEntries.push_back(resp);
    }
    Serial.printf("[%lu] [FS ] Directory has %u entries\n", millis(), (unsigned)dirEntries.size());
    dirIndex = 0;
    fileSizeBytes = 0;
  } else {
    open = true;
    fileSizeBytes = static_cast<size_t>(size);
  }
}

int FsFile::read(void* buf, size_t count) {
  if (!open || isDir) return -1;
  size_t bytesAvailable = (fileSizeBytes > filePos) ? (fileSizeBytes - filePos) : 0;
  if (bytesAvailable == 0) return 0;
  size_t toRead = std::min(count, bytesAvailable);
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
  std::string b64 = EmulationUtils::base64_encode((const char*)buffer, size);
  EmulationUtils::sendCmd(EmulationUtils::CMD_FS_WRITE, path.c_str(), b64.c_str(), String(filePos).c_str(), "1");
  EmulationUtils::recvRespInt64(); // unused for now
  filePos += size;
  if (filePos > fileSizeBytes) {
    fileSizeBytes = filePos;
  }
  return size;
}

size_t FsFile::write(uint8_t b) {
  return write(&b, 1);
}

#endif
