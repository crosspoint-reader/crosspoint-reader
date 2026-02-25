#include "KOReaderDocumentId.h"

#include <HalStorage.h>
#include <Logging.h>
#include <MD5Builder.h>

#include <functional>

namespace {
// Extract filename from path (everything after last '/')
std::string getFilename(const std::string& path) {
  const size_t pos = path.rfind('/');
  if (pos == std::string::npos) {
    return path;
  }
  return path.substr(pos + 1);
}
}  // namespace

std::string KOReaderDocumentId::getCacheFilePath(const std::string& filePath) {
  // Mirror the Epub cache directory convention so the hash file shares the
  // same per-book folder as other cached data.
  return std::string("/.crosspoint/epub_") +
         std::to_string(std::hash<std::string>{}(filePath)) +
         "/koreader_docid.txt";
}

std::string KOReaderDocumentId::loadCachedHash(const std::string& cacheFilePath,
                                                const size_t fileSize) {
  if (!Storage.exists(cacheFilePath.c_str())) {
    return "";
  }

  const String content = Storage.readFile(cacheFilePath.c_str());
  if (content.isEmpty()) {
    return "";
  }

  // Format: "<filesize>\n<32-char-hex-hash>"
  const int newlinePos = content.indexOf('\n');
  if (newlinePos < 0) {
    return "";
  }

  const size_t cachedSize = static_cast<size_t>(content.substring(0, newlinePos).toInt());
  if (cachedSize != fileSize) {
    LOG_DBG("KODoc", "Hash cache invalidated: file size changed (%zu -> %zu)", cachedSize, fileSize);
    return "";
  }

  std::string hash = content.substring(newlinePos + 1).c_str();
  // Trim any trailing whitespace / line endings
  while (!hash.empty() && (hash.back() == '\n' || hash.back() == '\r' || hash.back() == ' ')) {
    hash.pop_back();
  }

  if (hash.size() != 32) {
    return "";
  }

  LOG_DBG("KODoc", "Hash cache hit: %s", hash.c_str());
  return hash;
}

void KOReaderDocumentId::saveCachedHash(const std::string& cacheFilePath,
                                         const size_t fileSize,
                                         const std::string& hash) {
  // Ensure the book's cache directory exists before writing
  const size_t lastSlash = cacheFilePath.rfind('/');
  if (lastSlash != std::string::npos) {
    Storage.ensureDirectoryExists(cacheFilePath.substr(0, lastSlash).c_str());
  }

  String content(std::to_string(fileSize).c_str());
  content += '\n';
  content += hash.c_str();

  if (!Storage.writeFile(cacheFilePath.c_str(), content)) {
    LOG_DBG("KODoc", "Failed to write hash cache to %s", cacheFilePath.c_str());
  }
}

std::string KOReaderDocumentId::calculateFromFilename(const std::string& filePath) {
  const std::string filename = getFilename(filePath);
  if (filename.empty()) {
    return "";
  }

  MD5Builder md5;
  md5.begin();
  md5.add(filename.c_str());
  md5.calculate();

  std::string result = md5.toString().c_str();
  LOG_DBG("KODoc", "Filename hash: %s (from '%s')", result.c_str(), filename.c_str());
  return result;
}

size_t KOReaderDocumentId::getOffset(int i) {
  // Offset = 1024 << (2*i)
  // For i = -1: KOReader uses a value of 0
  // For i >= 0: 1024 << (2*i)
  if (i < 0) {
    return 0;
  }
  return CHUNK_SIZE << (2 * i);
}

std::string KOReaderDocumentId::calculate(const std::string& filePath) {
  FsFile file;
  if (!Storage.openFileForRead("KODoc", filePath, file)) {
    LOG_DBG("KODoc", "Failed to open file: %s", filePath.c_str());
    return "";
  }

  const size_t fileSize = file.fileSize();

  // Return persisted hash if the file size hasn't changed since it was cached
  const std::string cacheFilePath = getCacheFilePath(filePath);
  const std::string cached = loadCachedHash(cacheFilePath, fileSize);
  if (!cached.empty()) {
    file.close();
    return cached;
  }

  LOG_DBG("KODoc", "Calculating hash for file: %s (size: %zu)", filePath.c_str(), fileSize);

  // Initialize MD5 builder
  MD5Builder md5;
  md5.begin();

  // Buffer for reading chunks
  uint8_t buffer[CHUNK_SIZE];
  size_t totalBytesRead = 0;

  // Read from each offset (i = -1 to 10)
  for (int i = -1; i < OFFSET_COUNT - 1; i++) {
    const size_t offset = getOffset(i);

    // Skip if offset is beyond file size
    if (offset >= fileSize) {
      continue;
    }

    // Seek to offset
    if (!file.seekSet(offset)) {
      LOG_DBG("KODoc", "Failed to seek to offset %zu", offset);
      continue;
    }

    // Read up to CHUNK_SIZE bytes
    const size_t bytesToRead = std::min(CHUNK_SIZE, fileSize - offset);
    const size_t bytesRead = file.read(buffer, bytesToRead);

    if (bytesRead > 0) {
      md5.add(buffer, bytesRead);
      totalBytesRead += bytesRead;
    }
  }

  file.close();

  // Calculate final hash
  md5.calculate();
  std::string result = md5.toString().c_str();

  LOG_DBG("KODoc", "Hash calculated: %s (from %zu bytes)", result.c_str(), totalBytesRead);

  saveCachedHash(cacheFilePath, fileSize, result);

  return result;
}
