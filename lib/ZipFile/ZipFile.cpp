#include "ZipFile.h"

#include <HardwareSerial.h>
#include <miniz.h>

bool inflateOneShot(const uint8_t* inputBuf, const size_t deflatedSize, uint8_t* outputBuf, const size_t inflatedSize) {
  // Setup inflator
  const auto inflator = static_cast<tinfl_decompressor*>(malloc(sizeof(tinfl_decompressor)));
  if (!inflator) {
    Serial.printf("[%lu] [ZIP] Failed to allocate memory for inflator\n", millis());
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
    Serial.printf("[%lu] [ZIP] tinfl_decompress() failed with status %d\n", millis(), status);
    return false;
  }

  return true;
}

bool ZipFile::loadFileStat(const char* filename, mz_zip_archive_file_stat* fileStat) const {
  mz_zip_archive zipArchive = {};
  const bool status = mz_zip_reader_init_file(&zipArchive, filePath.c_str(), 0);

  if (!status) {
    Serial.printf("[%lu] [ZIP] mz_zip_reader_init_file() failed! Error: %s\n", millis(),
                  mz_zip_get_error_string(zipArchive.m_last_error));
    return false;
  }

  // find the file
  mz_uint32 fileIndex = 0;
  if (!mz_zip_reader_locate_file_v2(&zipArchive, filename, nullptr, 0, &fileIndex)) {
    Serial.printf("[%lu] [ZIP] Could not find file %s\n", millis(), filename);
    mz_zip_reader_end(&zipArchive);
    return false;
  }

  if (!mz_zip_reader_file_stat(&zipArchive, fileIndex, fileStat)) {
    Serial.printf("[%lu] [ZIP] mz_zip_reader_file_stat() failed! Error: %s\n", millis(),
                  mz_zip_get_error_string(zipArchive.m_last_error));
    mz_zip_reader_end(&zipArchive);
    return false;
  }
  mz_zip_reader_end(&zipArchive);
  return true;
}

long ZipFile::getDataOffset(const mz_zip_archive_file_stat& fileStat) const {
  constexpr auto localHeaderSize = 30;

  uint8_t pLocalHeader[localHeaderSize];
  const uint64_t fileOffset = fileStat.m_local_header_ofs;

  FILE* file = fopen(filePath.c_str(), "r");
  fseek(file, fileOffset, SEEK_SET);
  const size_t read = fread(pLocalHeader, 1, localHeaderSize, file);
  fclose(file);

  if (read != localHeaderSize) {
    Serial.printf("[%lu] [ZIP] Something went wrong reading the local header\n", millis());
    return -1;
  }

  if (pLocalHeader[0] + (pLocalHeader[1] << 8) + (pLocalHeader[2] << 16) + (pLocalHeader[3] << 24) !=
      0x04034b50 /* MZ_ZIP_LOCAL_DIR_HEADER_SIG */) {
    Serial.printf("[%lu] [ZIP] Not a valid zip file header\n", millis());
    return -1;
  }

  const uint16_t filenameLength = pLocalHeader[26] + (pLocalHeader[27] << 8);
  const uint16_t extraOffset = pLocalHeader[28] + (pLocalHeader[29] << 8);
  return fileOffset + localHeaderSize + filenameLength + extraOffset;
}

bool ZipFile::getInflatedFileSize(const char* filename, size_t* size) const {
  mz_zip_archive_file_stat fileStat;
  if (!loadFileStat(filename, &fileStat)) {
    return false;
  }

  *size = static_cast<size_t>(fileStat.m_uncomp_size);
  return true;
}

uint8_t* ZipFile::readFileToMemory(const char* filename, size_t* size, const bool trailingNullByte) const {
  mz_zip_archive_file_stat fileStat;
  if (!loadFileStat(filename, &fileStat)) {
    return nullptr;
  }

  const long fileOffset = getDataOffset(fileStat);
  if (fileOffset < 0) {
    return nullptr;
  }

  FILE* file = fopen(filePath.c_str(), "rb");
  fseek(file, fileOffset, SEEK_SET);

  const auto deflatedDataSize = static_cast<size_t>(fileStat.m_comp_size);
  const auto inflatedDataSize = static_cast<size_t>(fileStat.m_uncomp_size);
  const auto dataSize = trailingNullByte ? inflatedDataSize + 1 : inflatedDataSize;
  const auto data = static_cast<uint8_t*>(malloc(dataSize));

  if (fileStat.m_method == MZ_NO_COMPRESSION) {
    // no deflation, just read content
    const size_t dataRead = fread(data, 1, inflatedDataSize, file);
    fclose(file);

    if (dataRead != inflatedDataSize) {
      Serial.printf("[%lu] [ZIP] Failed to read data\n", millis());
      free(data);
      return nullptr;
    }

    // Continue out of block with data set
  } else if (fileStat.m_method == MZ_DEFLATED) {
    // Read out deflated content from file
    const auto deflatedData = static_cast<uint8_t*>(malloc(deflatedDataSize));
    if (deflatedData == nullptr) {
      Serial.printf("[%lu] [ZIP] Failed to allocate memory for decompression buffer\n", millis());
      fclose(file);
      return nullptr;
    }

    const size_t dataRead = fread(deflatedData, 1, deflatedDataSize, file);
    fclose(file);

    if (dataRead != deflatedDataSize) {
      Serial.printf("[%lu] [ZIP] Failed to read data, expected %d got %d\n", millis(), deflatedDataSize, dataRead);
      free(deflatedData);
      free(data);
      return nullptr;
    }

    bool success = inflateOneShot(deflatedData, deflatedDataSize, data, inflatedDataSize);
    free(deflatedData);

    if (!success) {
      Serial.printf("[%lu] [ZIP] Failed to inflate file\n", millis());
      free(data);
      return nullptr;
    }

    // Continue out of block with data set
  } else {
    Serial.printf("[%lu] [ZIP] Unsupported compression method\n", millis());
    fclose(file);
    return nullptr;
  }

  if (trailingNullByte) data[inflatedDataSize] = '\0';
  if (size) *size = inflatedDataSize;
  return data;
}

bool ZipFile::readFileToStream(const char* filename, Print& out, const size_t chunkSize) const {
  mz_zip_archive_file_stat fileStat;
  if (!loadFileStat(filename, &fileStat)) {
    return false;
  }

  const long fileOffset = getDataOffset(fileStat);
  if (fileOffset < 0) {
    return false;
  }

  FILE* file = fopen(filePath.c_str(), "rb");
  fseek(file, fileOffset, SEEK_SET);

  const auto deflatedDataSize = static_cast<size_t>(fileStat.m_comp_size);
  const auto inflatedDataSize = static_cast<size_t>(fileStat.m_uncomp_size);

  if (fileStat.m_method == MZ_NO_COMPRESSION) {
    // no deflation, just read content
    const auto buffer = static_cast<uint8_t*>(malloc(chunkSize));
    if (!buffer) {
      Serial.printf("[%lu] [ZIP] Failed to allocate memory for buffer\n", millis());
      fclose(file);
      return false;
    }

    size_t remaining = inflatedDataSize;
    while (remaining > 0) {
      const size_t dataRead = fread(buffer, 1, remaining < chunkSize ? remaining : chunkSize, file);
      if (dataRead == 0) {
        Serial.printf("[%lu] [ZIP] Could not read more bytes\n", millis());
        free(buffer);
        fclose(file);
        return false;
      }

      out.write(buffer, dataRead);
      remaining -= dataRead;
    }

    fclose(file);
    free(buffer);
    return true;
  }

  if (fileStat.m_method == MZ_DEFLATED) {
    // Setup inflator
    const auto inflator = static_cast<tinfl_decompressor*>(malloc(sizeof(tinfl_decompressor)));
    if (!inflator) {
      Serial.printf("[%lu] [ZIP] Failed to allocate memory for inflator\n", millis());
      fclose(file);
      return false;
    }
    memset(inflator, 0, sizeof(tinfl_decompressor));
    tinfl_init(inflator);

    // Setup file read buffer
    const auto fileReadBuffer = static_cast<uint8_t*>(malloc(chunkSize));
    if (!fileReadBuffer) {
      Serial.printf("[%lu] [ZIP] Failed to allocate memory for zip file read buffer\n", millis());
      free(inflator);
      fclose(file);
      return false;
    }

    const auto outputBuffer = static_cast<uint8_t*>(malloc(TINFL_LZ_DICT_SIZE));
    if (!outputBuffer) {
      Serial.printf("[%lu] [ZIP] Failed to allocate memory for dictionary\n", millis());
      free(inflator);
      free(fileReadBuffer);
      fclose(file);
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
            fread(fileReadBuffer, 1, fileRemainingBytes < chunkSize ? fileRemainingBytes : chunkSize, file);
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
          Serial.printf("[%lu] [ZIP] Failed to write all output bytes to stream\n", millis());
          fclose(file);
          free(outputBuffer);
          free(fileReadBuffer);
          free(inflator);
          return false;
        }
        // Update output position in buffer (with wraparound)
        outputCursor = (outputCursor + outBytes) & (TINFL_LZ_DICT_SIZE - 1);
      }

      if (status < 0) {
        Serial.printf("[%lu] [ZIP] tinfl_decompress() failed with status %d\n", millis(), status);
        fclose(file);
        free(outputBuffer);
        free(fileReadBuffer);
        free(inflator);
        return false;
      }

      if (status == TINFL_STATUS_DONE) {
        Serial.printf("[%lu] [ZIP] Decompressed %d bytes into %d bytes\n", millis(), deflatedDataSize,
                      inflatedDataSize);
        fclose(file);
        free(inflator);
        free(fileReadBuffer);
        free(outputBuffer);
        return true;
      }
    }

    // If we get here, EOF reached without TINFL_STATUS_DONE
    Serial.printf("[%lu] [ZIP] Unexpected EOF\n", millis());
    fclose(file);
    free(outputBuffer);
    free(fileReadBuffer);
    free(inflator);
    return false;
  }

  Serial.printf("[%lu] [ZIP] Unsupported compression method\n", millis());
  return false;
}
