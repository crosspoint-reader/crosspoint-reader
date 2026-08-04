#include "KOReaderDocumentId.h"

#include <HalStorage.h>
#include <Logging.h>
#include <MD5Builder.h>
#include <Print.h>
#include <ZipFile.h>

#include "Epub/parsers/ContainerParser.h"
#include "OriginalDocumentIdParser.h"

namespace {
constexpr char CONTAINER_PATH[] = "META-INF/container.xml";

// Extract filename from path (everything after last '/')
std::string getFilename(const std::string& path) {
  const size_t pos = path.rfind('/');
  if (pos == std::string::npos) {
    return path;
  }
  return path.substr(pos + 1);
}

class OriginalDocumentIdSink final : public Print {
  OriginalDocumentIdParser& parser;

 public:
  explicit OriginalDocumentIdSink(OriginalDocumentIdParser& parser) : parser(parser) {}

  size_t write(const uint8_t byte) override { return write(&byte, 1); }

  size_t write(const uint8_t* buffer, const size_t size) override {
    const size_t written = parser.write(buffer, size);
    // Returning a short write asks ZipFile to stop inflating once the metadata
    // has been found. The caller enables allowEarlyStop for this stream.
    return !parser.documentId.empty() ? 0 : written;
  }
};

std::string findEmbeddedOriginalDocumentId(const std::string& filePath) {
  ZipFile epub(filePath);

  size_t containerSize = 0;
  if (!epub.getInflatedFileSize(CONTAINER_PATH, &containerSize)) {
    return {};
  }

  ContainerParser containerParser(containerSize);
  if (!containerParser.setup() || !epub.readFileToStream(CONTAINER_PATH, containerParser, 512) ||
      containerParser.fullPath.empty()) {
    return {};
  }

  size_t opfSize = 0;
  if (!epub.getInflatedFileSize(containerParser.fullPath.c_str(), &opfSize)) {
    return {};
  }

  OriginalDocumentIdParser documentIdParser(opfSize);
  if (!documentIdParser.setup()) {
    return {};
  }

  OriginalDocumentIdSink sink(documentIdParser);
  if (!epub.readFileToStream(containerParser.fullPath.c_str(), sink, 512, true)) {
    return {};
  }
  return documentIdParser.documentId;
}
}  // namespace

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
  const std::string originalDocumentId = findEmbeddedOriginalDocumentId(filePath);
  if (!originalDocumentId.empty()) {
    LOG_DBG("KODoc", "Using embedded original document hash: %s", originalDocumentId.c_str());
    return originalDocumentId;
  }

  HalFile file;
  if (!Storage.openFileForRead("KODoc", filePath, file)) {
    LOG_DBG("KODoc", "Failed to open file: %s", filePath.c_str());
    return "";
  }

  const size_t fileSize = file.fileSize();
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

  // Calculate final hash
  md5.calculate();
  std::string result = md5.toString().c_str();

  LOG_DBG("KODoc", "Hash calculated: %s (from %zu bytes)", result.c_str(), totalBytesRead);

  return result;
}
