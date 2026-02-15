#include "ZipFile.h"

#include <HalStorage.h>
#include <Logging.h>
#include <miniz.h>

#include <algorithm>

bool inflateOneShot(const uint8_t* inputBuf, const size_t deflatedSize, uint8_t* outputBuf, const size_t inflatedSize) {
  // Setup inflator
  const auto inflator = static_cast<tinfl_decompressor*>(malloc(sizeof(tinfl_decompressor)));
  if (!inflator) {
    LOG_ERR("ZIP", "Failed to allocate memory for inflator");
    return false;
  }
  memset(inflator, 0, sizeof(tinfl_decompressor));
  tinfl_init(inflator);

  size_t inBytes = deflatedSize;
  size_t outBytes = inflatedSize;
  const tinfl_status status = tinfl_decompress(inflator, inputBuf, &inBytes, nullptr, outputBuf, &outBytes,
                                               TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  free(inflator);

  if (status != TINFL_STATUS_DONE) {
    LOG_ERR("ZIP", "tinfl_decompress() failed with status %d", status);
    return false;
  }

  return true;
}

bool ZipFile::loadAllFileStatSlims() {
  const bool wasOpen = isOpen();
  if (!wasOpen && !open()) {
    return false;
  }

  if (!loadZipDetails()) {
    if (!wasOpen) {
      close();
    }
    return false;
  }

  file.seek(zipDetails.centralDirOffset);

  uint32_t sig;
  char itemName[256];
  fileStatSlimCache.clear();
  fileStatSlimCache.reserve(zipDetails.totalEntries);

  while (file.available()) {
    file.read(&sig, 4);
    if (sig != 0x02014b50) break;  // End of list

    FileStatSlim fileStat = {};

    file.seekCur(6);
    file.read(&fileStat.method, 2);
    file.seekCur(8);
    file.read(&fileStat.compressedSize, 4);
    file.read(&fileStat.uncompressedSize, 4);
    uint16_t nameLen, m, k;
    file.read(&nameLen, 2);
    file.read(&m, 2);
    file.read(&k, 2);
    file.seekCur(8);
    file.read(&fileStat.localHeaderOffset, 4);
    file.read(itemName, nameLen);
    itemName[nameLen] = '\0';

    fileStatSlimCache.emplace(itemName, fileStat);

    // Skip the rest of this entry (extra field + comment)
    file.seekCur(m + k);
  }

  // Set cursor to start of central directory for sequential access
  lastCentralDirPos = zipDetails.centralDirOffset;
  lastCentralDirPosValid = true;

  if (!wasOpen) {
    close();
  }
  return true;
}

bool ZipFile::loadFileStatSlim(const char* filename, FileStatSlim* fileStat) {
  if (!fileStatSlimCache.empty()) {
    const auto it = fileStatSlimCache.find(filename);
    if (it != fileStatSlimCache.end()) {
      *fileStat = it->second;
      return true;
    }
    return false;
  }

  const bool wasOpen = isOpen();
  if (!wasOpen && !open()) {
    return false;
  }

  if (!loadZipDetails()) {
    if (!wasOpen) {
      close();
    }
    return false;
  }

  // Phase 1: Try scanning from cursor position first
  uint32_t startPos = lastCentralDirPosValid ? lastCentralDirPos : zipDetails.centralDirOffset;
  uint32_t wrapPos = zipDetails.centralDirOffset;
  bool wrapped = false;
  bool found = false;

  file.seek(startPos);

  uint32_t sig;
  char itemName[256];

  while (true) {
    uint32_t entryStart = file.position();

    if (file.read(&sig, 4) != 4 || sig != 0x02014b50) {
      // End of central directory
      if (!wrapped && lastCentralDirPosValid && startPos != zipDetails.centralDirOffset) {
        // Wrap around to beginning
        file.seek(zipDetails.centralDirOffset);
        wrapped = true;
        continue;
      }
      break;
    }

    // If we've wrapped and reached our start position, stop
    if (wrapped && entryStart >= startPos) {
      break;
    }

    file.seekCur(6);
    file.read(&fileStat->method, 2);
    file.seekCur(8);
    file.read(&fileStat->compressedSize, 4);
    file.read(&fileStat->uncompressedSize, 4);
    uint16_t nameLen, m, k;
    file.read(&nameLen, 2);
    file.read(&m, 2);
    file.read(&k, 2);
    file.seekCur(8);
    file.read(&fileStat->localHeaderOffset, 4);

    if (nameLen < 256) {
      file.read(itemName, nameLen);
      itemName[nameLen] = '\0';

      if (strcmp(itemName, filename) == 0) {
        // Found it! Update cursor to next entry
        file.seekCur(m + k);
        lastCentralDirPos = file.position();
        lastCentralDirPosValid = true;
        found = true;
        break;
      }
    } else {
      // Name too long, skip it
      file.seekCur(nameLen);
    }

    // Skip extra field + comment
    file.seekCur(m + k);
  }

  if (!wasOpen) {
    close();
  }
  return found;
}

long ZipFile::getDataOffset(const FileStatSlim& fileStat) {
  const bool wasOpen = isOpen();
  if (!wasOpen && !open()) {
    return -1;
  }

  constexpr auto localHeaderSize = 30;

  uint8_t pLocalHeader[localHeaderSize];
  const uint64_t fileOffset = fileStat.localHeaderOffset;

  file.seek(fileOffset);
  const size_t read = file.read(pLocalHeader, localHeaderSize);
  if (!wasOpen) {
    close();
  }

  if (read != localHeaderSize) {
    LOG_ERR("ZIP", "Something went wrong reading the local header");
    return -1;
  }

  if (pLocalHeader[0] + (pLocalHeader[1] << 8) + (pLocalHeader[2] << 16) + (pLocalHeader[3] << 24) !=
      0x04034b50 /* MZ_ZIP_LOCAL_DIR_HEADER_SIG */) {
    LOG_ERR("ZIP", "Not a valid zip file header");
    return -1;
  }

  const uint16_t filenameLength = pLocalHeader[26] + (pLocalHeader[27] << 8);
  const uint16_t extraOffset = pLocalHeader[28] + (pLocalHeader[29] << 8);
  return fileOffset + localHeaderSize + filenameLength + extraOffset;
}

bool ZipFile::loadZipDetails() {
  if (zipDetails.isSet) {
    return true;
  }

  const bool wasOpen = isOpen();
  if (!wasOpen && !open()) {
    return false;
  }

  const size_t fileSize = file.size();
  if (fileSize < 22) {
    LOG_ERR("ZIP", "File too small to be a valid zip");
    if (!wasOpen) {
      close();
    }
    return false;  // Minimum EOCD size is 22 bytes
  }

  // We scan the last 1KB (or the whole file if smaller) for the EOCD signature
  // 0x06054b50 is stored as 0x50, 0x4b, 0x05, 0x06 in little-endian
  const int scanRange = fileSize > 1024 ? 1024 : fileSize;
  const auto buffer = static_cast<uint8_t*>(malloc(scanRange));
  if (!buffer) {
    LOG_ERR("ZIP", "Failed to allocate memory for EOCD scan buffer");
    if (!wasOpen) {
      close();
    }
    return false;
  }

  file.seek(fileSize - scanRange);
  file.read(buffer, scanRange);

  // Scan backwards for the signature
  int foundOffset = -1;
  for (int i = scanRange - 22; i >= 0; i--) {
    constexpr uint32_t signature = 0x06054b50;
    if (*reinterpret_cast<uint32_t*>(&buffer[i]) == signature) {
      foundOffset = i;
      break;
    }
  }

  if (foundOffset == -1) {
    LOG_ERR("ZIP", "EOCD signature not found in zip file");
    free(buffer);
    if (!wasOpen) {
      close();
    }
    return false;
  }

  // Now extract the values we need from the EOCD record
  // Relative positions within EOCD:
  // Offset 10: Total number of entries (2 bytes)
  // Offset 16: Offset of start of central directory with respect to the starting disk number (4 bytes)
  zipDetails.totalEntries = *reinterpret_cast<uint16_t*>(&buffer[foundOffset + 10]);
  zipDetails.centralDirOffset = *reinterpret_cast<uint32_t*>(&buffer[foundOffset + 16]);
  zipDetails.isSet = true;

  free(buffer);
  if (!wasOpen) {
    close();
  }
  return true;
}

bool ZipFile::open() {
  if (!Storage.openFileForRead("ZIP", filePath, file)) {
    return false;
  }
  return true;
}

bool ZipFile::close() {
  if (file) {
    file.close();
  }
  lastCentralDirPos = 0;
  lastCentralDirPosValid = false;
  return true;
}

bool ZipFile::getInflatedFileSize(const char* filename, size_t* size) {
  FileStatSlim fileStat = {};
  if (!loadFileStatSlim(filename, &fileStat)) {
    return false;
  }

  *size = static_cast<size_t>(fileStat.uncompressedSize);
  return true;
}

int ZipFile::fillUncompressedSizes(std::vector<SizeTarget>& targets, std::vector<uint32_t>& sizes) {
  if (targets.empty()) {
    return 0;
  }

  const bool wasOpen = isOpen();
  if (!wasOpen && !open()) {
    return 0;
  }

  if (!loadZipDetails()) {
    if (!wasOpen) {
      close();
    }
    return 0;
  }

  file.seek(zipDetails.centralDirOffset);

  int matched = 0;
  uint32_t sig;
  char itemName[256];

  while (file.available()) {
    file.read(&sig, 4);
    if (sig != 0x02014b50) break;

    file.seekCur(6);
    uint16_t method;
    file.read(&method, 2);
    file.seekCur(8);
    uint32_t compressedSize, uncompressedSize;
    file.read(&compressedSize, 4);
    file.read(&uncompressedSize, 4);
    uint16_t nameLen, m, k;
    file.read(&nameLen, 2);
    file.read(&m, 2);
    file.read(&k, 2);
    file.seekCur(8);
    uint32_t localHeaderOffset;
    file.read(&localHeaderOffset, 4);

    if (nameLen < 256) {
      file.read(itemName, nameLen);
      itemName[nameLen] = '\0';

      uint64_t hash = fnvHash64(itemName, nameLen);
      SizeTarget key = {hash, nameLen, 0};

      auto it = std::lower_bound(targets.begin(), targets.end(), key, [](const SizeTarget& a, const SizeTarget& b) {
        return a.hash < b.hash || (a.hash == b.hash && a.len < b.len);
      });

      while (it != targets.end() && it->hash == hash && it->len == nameLen) {
        if (it->index < sizes.size()) {
          sizes[it->index] = uncompressedSize;
          matched++;
        }
        ++it;
      }
    } else {
      file.seekCur(nameLen);
    }

    file.seekCur(m + k);
  }

  if (!wasOpen) {
    close();
  }

  return matched;
}

uint8_t* ZipFile::readFileToMemory(const char* filename, size_t* size, const bool trailingNullByte) {
  const bool wasOpen = isOpen();
  if (!wasOpen && !open()) {
    return nullptr;
  }

  FileStatSlim fileStat = {};
  if (!loadFileStatSlim(filename, &fileStat)) {
    if (!wasOpen) {
      close();
    }
    return nullptr;
  }

  const long fileOffset = getDataOffset(fileStat);
  if (fileOffset < 0) {
    if (!wasOpen) {
      close();
    }
    return nullptr;
  }

  file.seek(fileOffset);

  const auto deflatedDataSize = fileStat.compressedSize;
  const auto inflatedDataSize = fileStat.uncompressedSize;
  const auto dataSize = trailingNullByte ? inflatedDataSize + 1 : inflatedDataSize;
  const auto data = static_cast<uint8_t*>(malloc(dataSize));
  if (data == nullptr) {
    LOG_ERR("ZIP", "Failed to allocate memory for output buffer (%zu bytes)", dataSize);
    if (!wasOpen) {
      close();
    }
    return nullptr;
  }

  if (fileStat.method == MZ_NO_COMPRESSION) {
    // no deflation, just read content
    const size_t dataRead = file.read(data, inflatedDataSize);
    if (!wasOpen) {
      close();
    }

    if (dataRead != inflatedDataSize) {
      LOG_ERR("ZIP", "Failed to read data");
      free(data);
      return nullptr;
    }

    // Continue out of block with data set
  } else if (fileStat.method == MZ_DEFLATED) {
    // Read out deflated content from file
    const auto deflatedData = static_cast<uint8_t*>(malloc(deflatedDataSize));
    if (deflatedData == nullptr) {
      LOG_ERR("ZIP", "Failed to allocate memory for decompression buffer");
      if (!wasOpen) {
        close();
      }
      return nullptr;
    }

    const size_t dataRead = file.read(deflatedData, deflatedDataSize);
    if (!wasOpen) {
      close();
    }

    if (dataRead != deflatedDataSize) {
      LOG_ERR("ZIP", "Failed to read data, expected %d got %d", deflatedDataSize, dataRead);
      free(deflatedData);
      free(data);
      return nullptr;
    }

    const bool success = inflateOneShot(deflatedData, deflatedDataSize, data, inflatedDataSize);
    free(deflatedData);

    if (!success) {
      LOG_ERR("ZIP", "Failed to inflate file");
      free(data);
      return nullptr;
    }

    // Continue out of block with data set
  } else {
    LOG_ERR("ZIP", "Unsupported compression method");
    if (!wasOpen) {
      close();
    }
    return nullptr;
  }

  if (trailingNullByte) data[inflatedDataSize] = '\0';
  if (size) *size = inflatedDataSize;
  return data;
}

bool ZipFile::readFileToStream(const char* filename, Print& out, const size_t chunkSize) {
  const bool wasOpen = isOpen();
  if (!wasOpen && !open()) {
    return false;
  }

  FileStatSlim fileStat = {};
  if (!loadFileStatSlim(filename, &fileStat)) {
    return false;
  }

  const long fileOffset = getDataOffset(fileStat);
  if (fileOffset < 0) {
    return false;
  }

  file.seek(fileOffset);
  const auto deflatedDataSize = fileStat.compressedSize;
  const auto inflatedDataSize = fileStat.uncompressedSize;

  if (fileStat.method == MZ_NO_COMPRESSION) {
    // no deflation, just read content
    const auto buffer = static_cast<uint8_t*>(malloc(chunkSize));
    if (!buffer) {
      LOG_ERR("ZIP", "Failed to allocate memory for buffer");
      if (!wasOpen) {
        close();
      }
      return false;
    }

    size_t remaining = inflatedDataSize;
    while (remaining > 0) {
      const size_t dataRead = file.read(buffer, remaining < chunkSize ? remaining : chunkSize);
      if (dataRead == 0) {
        LOG_ERR("ZIP", "Could not read more bytes");
        free(buffer);
        if (!wasOpen) {
          close();
        }
        return false;
      }

      out.write(buffer, dataRead);
      remaining -= dataRead;
    }

    if (!wasOpen) {
      close();
    }
    free(buffer);
    return true;
  }

  if (fileStat.method == MZ_DEFLATED) {
    // Setup inflator
    const auto inflator = static_cast<tinfl_decompressor*>(malloc(sizeof(tinfl_decompressor)));
    if (!inflator) {
      LOG_ERR("ZIP", "Failed to allocate memory for inflator");
      if (!wasOpen) {
        close();
      }
      return false;
    }
    memset(inflator, 0, sizeof(tinfl_decompressor));
    tinfl_init(inflator);

    // Setup file read buffer
    const auto fileReadBuffer = static_cast<uint8_t*>(malloc(chunkSize));
    if (!fileReadBuffer) {
      LOG_ERR("ZIP", "Failed to allocate memory for zip file read buffer");
      free(inflator);
      if (!wasOpen) {
        close();
      }
      return false;
    }

    const auto outputBuffer = static_cast<uint8_t*>(malloc(TINFL_LZ_DICT_SIZE));
    if (!outputBuffer) {
      LOG_ERR("ZIP", "Failed to allocate memory for dictionary");
      free(inflator);
      free(fileReadBuffer);
      if (!wasOpen) {
        close();
      }
      return false;
    }
    memset(outputBuffer, 0, TINFL_LZ_DICT_SIZE);

    size_t fileRemainingBytes = deflatedDataSize;
    size_t processedOutputBytes = 0;
    size_t fileReadBufferFilledBytes = 0;
    size_t fileReadBufferCursor = 0;
    size_t outputCursor = 0;  // Current offset in the circular dictionary

    while (true) {
      // Load more compressed bytes when needed
      if (fileReadBufferCursor >= fileReadBufferFilledBytes) {
        if (fileRemainingBytes == 0) {
          // Should not be hit, but a safe protection
          break;  // EOF
        }

        fileReadBufferFilledBytes =
            file.read(fileReadBuffer, fileRemainingBytes < chunkSize ? fileRemainingBytes : chunkSize);
        fileRemainingBytes -= fileReadBufferFilledBytes;
        fileReadBufferCursor = 0;

        if (fileReadBufferFilledBytes == 0) {
          // Bad read
          break;  // EOF
        }
      }

      // Available bytes in fileReadBuffer to process
      size_t inBytes = fileReadBufferFilledBytes - fileReadBufferCursor;
      // Space remaining in outputBuffer
      size_t outBytes = TINFL_LZ_DICT_SIZE - outputCursor;

      const tinfl_status status = tinfl_decompress(inflator, fileReadBuffer + fileReadBufferCursor, &inBytes,
                                                   outputBuffer, outputBuffer + outputCursor, &outBytes,
                                                   fileRemainingBytes > 0 ? TINFL_FLAG_HAS_MORE_INPUT : 0);

      // Update input position
      fileReadBufferCursor += inBytes;

      // Write output chunk
      if (outBytes > 0) {
        processedOutputBytes += outBytes;
        if (out.write(outputBuffer + outputCursor, outBytes) != outBytes) {
          LOG_ERR("ZIP", "Failed to write all output bytes to stream");
          if (!wasOpen) {
            close();
          }
          free(outputBuffer);
          free(fileReadBuffer);
          free(inflator);
          return false;
        }
        // Update output position in buffer (with wraparound)
        outputCursor = (outputCursor + outBytes) & (TINFL_LZ_DICT_SIZE - 1);
      }

      if (status < 0) {
        LOG_ERR("ZIP", "tinfl_decompress() failed with status %d", status);
        if (!wasOpen) {
          close();
        }
        free(outputBuffer);
        free(fileReadBuffer);
        free(inflator);
        return false;
      }

      if (status == TINFL_STATUS_DONE) {
        LOG_ERR("ZIP", "Decompressed %d bytes into %d bytes", deflatedDataSize, inflatedDataSize);
        if (!wasOpen) {
          close();
        }
        free(inflator);
        free(fileReadBuffer);
        free(outputBuffer);
        return true;
      }
    }

    // If we get here, EOF reached without TINFL_STATUS_DONE
    LOG_ERR("ZIP", "Unexpected EOF");
    if (!wasOpen) {
      close();
    }
    free(outputBuffer);
    free(fileReadBuffer);
    free(inflator);
    return false;
  }

  if (!wasOpen) {
    close();
  }

  LOG_ERR("ZIP", "Unsupported compression method");
  return false;
}
