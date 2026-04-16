#include "SDCardManager.h"

namespace {
constexpr uint8_t SD_CS = 12;
constexpr uint32_t SPI_FQ = 40000000;

void logSdPrefix() {
  if (!Serial) {
    return;
  }
  Serial.print('[');
  Serial.print(millis());
  Serial.print("] [SD] ");
}

void logSdLine(const char* msg) {
  if (!Serial) {
    return;
  }
  logSdPrefix();
  Serial.println(msg);
}

void logSdLine(const char* prefix, const char* value) {
  if (!Serial) {
    return;
  }
  logSdPrefix();
  Serial.print(prefix);
  Serial.println(value);
}

void logSdModuleLine(const char* moduleName, const char* msg) {
  if (!Serial) {
    return;
  }
  Serial.print('[');
  Serial.print(millis());
  Serial.print("] [");
  Serial.print(moduleName);
  Serial.print("] ");
  Serial.println(msg);
}

void logSdModulePath(const char* moduleName, const char* msg, const char* path) {
  if (!Serial) {
    return;
  }
  Serial.print('[');
  Serial.print(millis());
  Serial.print("] [");
  Serial.print(moduleName);
  Serial.print("] ");
  Serial.print(msg);
  Serial.println(path);
}
}  // namespace

SDCardManager SDCardManager::instance;

SDCardManager::SDCardManager() : sd() {}

bool SDCardManager::begin() {
  if (!sd.begin(SD_CS, SPI_FQ)) {
    logSdLine("SD card not detected");
    initialized = false;
  } else {
    logSdLine("SD card detected");
    initialized = true;
  }

  return initialized;
}

bool SDCardManager::ready() const { return initialized; }

std::vector<String> SDCardManager::listFiles(const char* path, const int maxFiles) {
  std::vector<String> ret;
  if (!initialized) {
    logSdLine("not initialized, returning empty list");
    return ret;
  }

  auto root = sd.open(path);
  if (!root) {
    logSdLine("Failed to open directory");
    return ret;
  }
  if (!root.isDirectory()) {
    logSdLine("Path is not a directory");
    root.close();
    return ret;
  }

  int count = 0;
  char name[128];
  for (auto f = root.openNextFile(); f && count < maxFiles; f = root.openNextFile()) {
    if (f.isDirectory()) {
      f.close();
      continue;
    }
    f.getName(name, sizeof(name));
    ret.emplace_back(name);
    f.close();
    count++;
  }
  root.close();
  return ret;
}

String SDCardManager::readFile(const char* path) {
  if (!initialized) {
    logSdLine("not initialized; cannot read file");
    return {""};
  }

  FsFile f;
  if (!openFileForRead("SD", path, f)) {
    return {""};
  }

  String content = "";
  constexpr size_t maxSize = 50000;  // Limit to 50KB
  size_t readSize = 0;
  while (f.available() && readSize < maxSize) {
    const char c = static_cast<char>(f.read());
    content += c;
    readSize++;
  }
  f.close();
  return content;
}

bool SDCardManager::readFileToStream(const char* path, Print& out, const size_t chunkSize) {
  if (!initialized) {
    logSdLine("Path is not a directory");
    logSdLine("SDCardManager: not initialized; cannot read file");
    return false;
  }

  FsFile f;
  if (!openFileForRead("SD", path, f)) {
    return false;
  }

  constexpr size_t localBufSize = 256;
  uint8_t buf[localBufSize];
  const size_t toRead = (chunkSize == 0) ? localBufSize : (chunkSize < localBufSize ? chunkSize : localBufSize);

  while (f.available()) {
    const int r = f.read(buf, toRead);
    if (r > 0) {
      out.write(buf, static_cast<size_t>(r));
    } else {
      break;
    }
  }

  f.close();
  return true;
}

size_t SDCardManager::readFileToBuffer(const char* path, char* buffer, const size_t bufferSize, const size_t maxBytes) {
  if (!buffer || bufferSize == 0) return 0;
  if (!initialized) {
    logSdLine("Path is not a directory");
    logSdLine("SDCardManager: not initialized; cannot read file");
    buffer[0] = '\0';
    return 0;
  }

  FsFile f;
  if (!openFileForRead("SD", path, f)) {
    buffer[0] = '\0';
    return 0;
  }

  const size_t maxToRead = (maxBytes == 0) ? (bufferSize - 1) : min(maxBytes, bufferSize - 1);
  size_t total = 0;

  while (f.available() && total < maxToRead) {
    constexpr size_t chunk = 64;
    const size_t want = maxToRead - total;
    const size_t readLen = (want < chunk) ? want : chunk;
    const int r = f.read(buffer + total, readLen);
    if (r > 0) {
      total += static_cast<size_t>(r);
    } else {
      break;
    }
  }

  buffer[total] = '\0';
  f.close();
  return total;
}

bool SDCardManager::writeFile(const char* path, const String& content) {
  if (!initialized) {
    logSdLine("Path is not a directory");
    logSdLine("SDCardManager: not initialized; cannot write file");
    return false;
  }

  // Remove existing file so we perform an overwrite rather than append
  if (sd.exists(path)) {
    sd.remove(path);
  }

  FsFile f;
  if (!openFileForWrite("SD", path, f)) {
    logSdLine("Path is not a directory");
    logSdLine("Failed to open file for write: ", path);
    return false;
  }

  const size_t written = f.print(content);
  f.close();
  return written == content.length();
}

bool SDCardManager::ensureDirectoryExists(const char* path) {
  if (!initialized) {
    logSdLine("Path is not a directory");
    logSdLine("SDCardManager: not initialized; cannot create directory");
    return false;
  }

  // Check if directory already exists
  if (sd.exists(path)) {
    FsFile dir = sd.open(path);
    if (dir && dir.isDirectory()) {
      dir.close();
      logSdLine("Directory already exists: ", path);
      return true;
    }
    dir.close();
  }

  // Create the directory
  if (sd.mkdir(path)) {
    logSdLine("Created directory: ", path);
    return true;
  } else {
    logSdLine("Failed to create directory: ", path);
    return false;
  }
}

bool SDCardManager::openFileForRead(const char* moduleName, const char* path, FsFile& file) {
  file = sd.open(path, O_RDONLY);
  if (!file) {
    logSdModulePath(moduleName, "Failed to open file for reading: ", path);
    return false;
  }
  return true;
}

bool SDCardManager::openFileForRead(const char* moduleName, const std::string& path, FsFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool SDCardManager::openFileForRead(const char* moduleName, const String& path, FsFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool SDCardManager::openFileForWrite(const char* moduleName, const char* path, FsFile& file) {
  file = sd.open(path, O_RDWR | O_CREAT | O_TRUNC);
  if (!file) {
    logSdModulePath(moduleName, "Failed to open file for writing: ", path);
    return false;
  }
  return true;
}

bool SDCardManager::openFileForWrite(const char* moduleName, const std::string& path, FsFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool SDCardManager::openFileForWrite(const char* moduleName, const String& path, FsFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool SDCardManager::removeDir(const char* path) {
  // 1. Open the directory
  auto dir = sd.open(path);
  if (!dir) {
    return false;
  }
  if (!dir.isDirectory()) {
    return false;
  }

  auto file = dir.openNextFile();
  char name[128];
  while (file) {
    String filePath = path;
    if (!filePath.endsWith("/")) {
      filePath += "/";
    }
    file.getName(name, sizeof(name));
    filePath += name;

    if (file.isDirectory()) {
      if (!removeDir(filePath.c_str())) {
        return false;
      }
    } else {
      if (!sd.remove(filePath.c_str())) {
        return false;
      }
    }
    file = dir.openNextFile();
  }

  return sd.rmdir(path);
}
