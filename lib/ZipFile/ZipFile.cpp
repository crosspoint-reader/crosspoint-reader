#include "ZipFile.h"

#include <BufferedFileIO.h>
#include <BuildArena.h>
#include <HalStorage.h>
#include <InflateReader.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>

#include "../Epub/Epub/HashUtils.h"

struct ZipInflateCtx {
  InflateReader reader;  // Must be first — callback casts uzlib_uncomp* to ZipInflateCtx*
  FsFile* file = nullptr;
  size_t fileRemaining = 0;
  uint8_t* readBuf = nullptr;
  size_t readBufSize = 0;
};

namespace {
constexpr uint16_t ZIP_METHOD_STORED = 0;
constexpr uint16_t ZIP_METHOD_DEFLATED = 8;

int zipReadCallback(uzlib_uncomp* uncomp) {
  auto* ctx = reinterpret_cast<ZipInflateCtx*>(uncomp);
  if (ctx->fileRemaining == 0) return -1;

  const size_t toRead = ctx->fileRemaining < ctx->readBufSize ? ctx->fileRemaining : ctx->readBufSize;
  // Ported from crosspoint-reader PR #3244 ("prevent size_t underflow on ZipFile read errors",
  // Foulad / @sfoulad). The defect and the fix are theirs; the end-of-stream signal differs
  // because this is a uzlib READ callback -- it returns the next byte, so 0 is data and -1 is
  // the only way to say "no more". Upstream's fill callback returns a byte COUNT, where their
  // `return 0` is the equivalent signal.
  const int result = ctx->file->read(ctx->readBuf, toRead);
  if (result < 0) {
    // HalFile::read() reports errors as a negative int. Assigning that straight to size_t
    // underflowed fileRemaining and produced a huge bytesRead, which pushed source_limit far
    // past the end of readBuf and had uzlib inflate out of bounds.
    LOG_ERR("ZIP", "Failed to read compressed data: %d", result);
    return -1;
  }
  const size_t bytesRead = static_cast<size_t>(result);
  ctx->fileRemaining -= bytesRead;

  if (bytesRead == 0) return -1;

  uncomp->source = ctx->readBuf + 1;
  uncomp->source_limit = ctx->readBuf + bytesRead;
  return ctx->readBuf[0];
}
}  // namespace

static std::string normalizeZipPath(const char* filename) {
  std::string normalized;
  normalized.reserve(strlen(filename));
  for (const char* p = filename; *p; ++p) {
    if (*p == '\\') {
      normalized.push_back('/');
    } else {
      normalized.push_back(*p);
    }
  }
  while (!normalized.empty() && normalized.front() == '/') {
    normalized.erase(normalized.begin());
  }
  return normalized;
}

static std::string normalizeZipPathLower(const char* filename) {
  std::string normalized = normalizeZipPath(filename);
  for (char& ch : normalized) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return normalized;
}

bool ZipFile::loadFileStatSlim(const char* filename, FileStatSlim* fileStat) {
  const std::string normalizedFilename = normalizeZipPath(filename);
  const std::string normalizedFilenameLower = normalizeZipPathLower(filename);

  const ScopedOpenClose zip{*this};
  if (!zip) {
    LOG_ERR("ZIP", "loadFileStatSlim: failed to open zip for %s", filename);
    return false;
  }

  if (!loadZipDetails()) {
    LOG_ERR("ZIP", "loadFileStatSlim: loadZipDetails failed for %s", filename);
    return false;
  }

  // Phase 1: Try scanning from cursor position first
  uint32_t startPos = lastCentralDirPosValid ? lastCentralDirPos : zipDetails.centralDirOffset;
  bool wrapped = false;
  bool found = false;

  file.seek(startPos);

  uint32_t sig;
  char itemName[256];
#ifdef BENCH_EXTRACT_PROFILE
  uint32_t scanned = 0;
#endif

  while (true) {
#ifdef BENCH_EXTRACT_PROFILE
    ++scanned;
#endif
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

      // Normalize once, then build lowercase via in-place transform — one alloc, not two.
      std::string normalizedItemName = normalizeZipPath(itemName);
      if (normalizedItemName == normalizedFilename) {
        file.seekCur(m + k);
        lastCentralDirPos = file.position();
        lastCentralDirPosValid = true;
        found = true;
        break;
      }

      std::string normalizedItemNameLower = normalizedItemName;
      for (char& ch : normalizedItemNameLower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      if (normalizedItemNameLower == normalizedFilenameLower) {
        LOG_DBG("ZIP", "loadFileStatSlim: case-insensitive match for %s => %s", filename, itemName);
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

  if (!found) {
    LOG_DBG("ZIP", "loadFileStatSlim: entry not found after scan: %s (normalized=%s)", filename,
            normalizedFilename.c_str());
  }
#ifdef BENCH_EXTRACT_PROFILE
  LOG_INF("ZIP", "STATSCAN entries=%lu wrapped=%d found=%d", static_cast<unsigned long>(scanned), wrapped ? 1 : 0,
          found ? 1 : 0);
#endif

  return found;
}

bool ZipFile::getStoredEntryRange(const char* filename, uint32_t* offset, uint32_t* size) {
  if (!offset || !size) return false;
  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  FileStatSlim fileStat = {};
  if (!loadFileStatSlim(filename, &fileStat)) return false;
  if (fileStat.method != ZIP_METHOD_STORED) return false;

  const long dataOffset = getDataOffset(fileStat);
  if (dataOffset < 0) return false;

  *offset = static_cast<uint32_t>(dataOffset);
  *size = fileStat.uncompressedSize;  // identical to compressedSize for a stored entry
  return true;
}

long ZipFile::getDataOffset(const FileStatSlim& fileStat) {
  const ScopedOpenClose zip{*this};
  if (!zip) return -1;

  constexpr auto localHeaderSize = 30;

  uint8_t pLocalHeader[localHeaderSize];
  const uint64_t fileOffset = fileStat.localHeaderOffset;

  file.seek(fileOffset);
  const size_t read = file.read(pLocalHeader, localHeaderSize);

  if (read != localHeaderSize) {
    LOG_ERR("ZIP", "Something went wrong reading the local header");
    return -1;
  }

  if (pLocalHeader[0] + (pLocalHeader[1] << 8) + (pLocalHeader[2] << 16) + (pLocalHeader[3] << 24) !=
      0x04034b50 /* ZIP local file header signature */) {
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

  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  const size_t fileSize = file.size();
  if (fileSize < 22) {
    LOG_ERR("ZIP", "File too small to be a valid zip");
    return false;  // Minimum EOCD size is 22 bytes
  }

  // Scan backwards from end-of-file for the EOCD signature (0x06054b50).
  // ZIP spec allows up to 65535+22 bytes of comment after EOCD, so the
  // signature can be up to 65557 bytes from the end.  To avoid a large
  // heap allocation on the memory-constrained ESP32-C3, we use a fixed
  // 4KB window that starts at end-of-file and steps backwards only when
  // no signature is found, checking the window nearest EOF first.  This
  // guarantees we resolve to the EOCD nearest EOF (the correct one per
  // spec) instead of a false PK\x05\x06 sequence earlier in the file,
  // and it keeps the common case (no archive comment) to a single 4KB
  // read instead of scanning the full 65557-byte region.  Each step
  // overlaps the previous window by 21 bytes so an EOCD record spanning
  // a window boundary is never missed (EOCD minimum size is 22 bytes).
  constexpr size_t BUF_SIZE = 4096;
  constexpr size_t MAX_SCAN = 65557;
  constexpr size_t OVERLAP = 21;  // EOCD min size - 1

  auto buffer = makeUniqueNoThrow<uint8_t[]>(BUF_SIZE);
  if (!buffer) {
    LOG_ERR("ZIP", "Failed to allocate EOCD scan buffer (%zu bytes)", BUF_SIZE);
    return false;
  }

  const size_t totalScannable = fileSize < MAX_SCAN ? fileSize : MAX_SCAN;
  const size_t scanFloor = fileSize - totalScannable;

  size_t windowEnd = fileSize;
  while (true) {
    const size_t windowStart = windowEnd > scanFloor + BUF_SIZE ? windowEnd - BUF_SIZE : scanFloor;
    const size_t windowLen = windowEnd - windowStart;
    if (windowLen < 22) break;  // Not enough bytes left to hold an EOCD record

    if (!file.seek(windowStart)) {
      LOG_ERR("ZIP", "EOCD scan: seek to %zu failed", windowStart);
      return false;
    }

    size_t filled = 0;
    while (filled < windowLen) {
      const int n = file.read(buffer.get() + filled, windowLen - filled);
      if (n <= 0) {
        LOG_ERR("ZIP", "EOCD scan: read failed in window [%zu, %zu), got %zu/%zu bytes", windowStart, windowEnd, filled,
                windowLen);
        return false;
      }
      filled += static_cast<size_t>(n);
    }

    // Search this window from the end towards the start so the match
    // nearest EOF wins within the window (only one EOCD exists per valid
    // ZIP, but this keeps behaviour well-defined if a comment happens to
    // contain the signature bytes).
    for (int i = static_cast<int>(windowLen) - 22; i >= 0; i--) {
      uint32_t candidate;
      memcpy(&candidate, &buffer[i], sizeof(candidate));
      if (candidate == 0x06054b50) {
        memcpy(&zipDetails.totalEntries, &buffer[i + 10], sizeof(zipDetails.totalEntries));
        memcpy(&zipDetails.centralDirOffset, &buffer[i + 16], sizeof(zipDetails.centralDirOffset));
        zipDetails.isSet = true;
        LOG_DBG("ZIP", "EOCD found at offset %zu in file", windowStart + static_cast<size_t>(i));
        return true;
      }
    }

    if (windowStart <= scanFloor) break;
    windowEnd = windowStart + OVERLAP;
  }

  LOG_ERR("ZIP", "EOCD signature not found in zip file (scanned last %zu bytes)", totalScannable);
  return false;
}

bool ZipFile::contentFingerprint(uint64_t* out) {
  if (!out) return false;
  if (!loadZipDetails()) return false;
  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  // Incremental FNV-1a 64 mixing helpers (same constants as HashUtils).
  uint64_t hash = 14695981039346656037ull;
  const auto mixBytes = [&hash](const void* data, const size_t len) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; i++) {
      hash ^= p[i];
      hash *= 1099511628211ull;
    }
  };
  const auto mixPod = [&mixBytes](const auto v) { mixBytes(&v, sizeof(v)); };

  // Buffered walk: the per-entry field reads below would otherwise be ~12
  // separate FsFile calls per entry (~1.5 ms each on device — seconds for a
  // 1000+-entry book). Through the 4 KB window they collapse into a few
  // sequential SD reads; the per-entry skips stay inside the window for free.
  file.seek(zipDetails.centralDirOffset);
  serialization::BufferedFileReader reader(file);
  char nameBuf[256];
  uint32_t entries = 0;
  uint32_t sig;
  while (reader.readPod(sig) && sig == 0x02014b50) {
    reader.seek(reader.position() + 6);  // versionMadeBy, versionNeeded, flags
    uint16_t method, nameLen, extraLen, commentLen;
    uint32_t crc32, compSz, uncompSz, localOff;
    if (!reader.readPod(method)) return false;
    // DOS mod time + date: excluded so a content-identical re-zip matches
    reader.seek(reader.position() + 4);
    if (!reader.readPod(crc32) || !reader.readPod(compSz) || !reader.readPod(uncompSz) || !reader.readPod(nameLen) ||
        !reader.readPod(extraLen) || !reader.readPod(commentLen)) {
      return false;
    }
    reader.seek(reader.position() + 8);  // disk#, internal attrs, external attrs
    if (!reader.readPod(localOff)) return false;

    // Name in bounded chunks: entry names may exceed the stack buffer.
    size_t nameRemaining = nameLen;
    while (nameRemaining > 0) {
      const size_t take = nameRemaining < sizeof(nameBuf) ? nameRemaining : sizeof(nameBuf);
      if (!reader.read(nameBuf, take)) return false;
      mixBytes(nameBuf, take);
      nameRemaining -= take;
    }
    mixPod(crc32);
    mixPod(uncompSz);
    mixPod(method);
    mixPod(localOff);
    entries++;

    reader.seek(reader.position() + extraLen + commentLen);
  }
  mixPod(entries);
  *out = hash;
  return entries > 0;
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

int ZipFile::fillUncompressedSizes(const std::deque<SizeTarget>& targets, std::deque<uint32_t>& sizes) {
  if (targets.empty()) {
    return 0;
  }

  const ScopedOpenClose zip{*this};
  if (!zip) return 0;

  if (!loadZipDetails()) return 0;

  file.seek(zipDetails.centralDirOffset);

  int matched = 0;
  const int targetCount = static_cast<int>(targets.size());
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

      uint64_t hash = HashUtils::fnvHash64(itemName, nameLen);
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

      if (matched >= targetCount) {
        break;
      }
    } else {
      file.seekCur(nameLen);
    }

    file.seekCur(m + k);
  }

  return matched;
}

int ZipFile::fillFileStats(const std::deque<SizeTarget>& targets, std::deque<FileStatSlim>& stats) {
  if (targets.empty()) {
    return 0;
  }

  const ScopedOpenClose zip{*this};
  if (!zip) return 0;

  if (!loadZipDetails()) return 0;

  file.seek(zipDetails.centralDirOffset);

  int matched = 0;
  const int targetCount = static_cast<int>(targets.size());
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

      const uint64_t hash = HashUtils::fnvHash64(itemName, nameLen);
      const SizeTarget key = {hash, nameLen, 0};

      auto it = std::lower_bound(targets.begin(), targets.end(), key, [](const SizeTarget& a, const SizeTarget& b) {
        return a.hash < b.hash || (a.hash == b.hash && a.len < b.len);
      });

      while (it != targets.end() && it->hash == hash && it->len == nameLen) {
        if (it->index < stats.size()) {
          stats[it->index] = {method, compressedSize, uncompressedSize, localHeaderOffset};
          matched++;
        }
        ++it;
      }

      if (matched >= targetCount) {
        break;
      }
    } else {
      file.seekCur(nameLen);
    }

    file.seekCur(m + k);
  }

  return matched;
}

uint8_t* ZipFile::readFileToMemory(const char* filename, size_t* size, const bool trailingNullByte) {
  const ScopedOpenClose zip{*this};
  if (!zip) return nullptr;

  FileStatSlim fileStat = {};
  if (!loadFileStatSlim(filename, &fileStat)) return nullptr;

  const long fileOffset = getDataOffset(fileStat);
  if (fileOffset < 0) return nullptr;

  file.seek(fileOffset);

  const auto deflatedDataSize = fileStat.compressedSize;
  const auto inflatedDataSize = fileStat.uncompressedSize;
  // A corrupt entry (or a ZIP64 one, whose 32-bit size field holds the 0xFFFFFFFF marker) claims
  // 4 GB. With the trailing NUL that size wraps to a 0-byte allocation, which the read below would
  // then fill with the entry: a heap overflow from a crafted EPUB.
  if (trailingNullByte && inflatedDataSize == UINT32_MAX) {
    LOG_ERR("ZIP", "Entry claims %u bytes, refusing %s", static_cast<unsigned>(inflatedDataSize), filename);
    return nullptr;
  }
  const size_t dataSize = trailingNullByte ? size_t{inflatedDataSize} + 1 : inflatedDataSize;
  const auto data = static_cast<uint8_t*>(malloc(dataSize));
  if (data == nullptr) {
    LOG_ERR("ZIP", "Failed to allocate memory for output buffer (%zu bytes)", dataSize);
    return nullptr;
  }

  if (fileStat.method == ZIP_METHOD_STORED) {
    // no deflation, just read content
    const size_t dataRead = file.read(data, inflatedDataSize);

    if (dataRead != inflatedDataSize) {
      LOG_ERR("ZIP", "Failed to read data");
      free(data);
      return nullptr;
    }

    // Continue out of block with data set
  } else if (fileStat.method == ZIP_METHOD_DEFLATED) {
    // Read out deflated content from file
    const auto deflatedData = static_cast<uint8_t*>(malloc(deflatedDataSize));
    if (deflatedData == nullptr) {
      LOG_ERR("ZIP", "Failed to allocate memory for decompression buffer");
      free(data);
      return nullptr;
    }

    const size_t dataRead = file.read(deflatedData, deflatedDataSize);

    if (dataRead != deflatedDataSize) {
      LOG_ERR("ZIP", "Failed to read data, expected %d got %d", deflatedDataSize, dataRead);
      free(deflatedData);
      free(data);
      return nullptr;
    }

    bool success = false;
    {
      InflateReader r;
      r.init(false);
      r.setSource(deflatedData, deflatedDataSize);
      success = r.read(data, inflatedDataSize);
    }
    free(deflatedData);

    if (!success) {
      LOG_ERR("ZIP", "Failed to inflate file");
      free(data);
      return nullptr;
    }

    // Continue out of block with data set
  } else {
    LOG_ERR("ZIP", "Unsupported compression method");
    free(data);
    return nullptr;
  }

  if (trailingNullByte) data[inflatedDataSize] = '\0';
  if (size) *size = inflatedDataSize;
  return data;
}

bool ZipFile::readFileToStream(const char* filename, Print& out, const size_t chunkSize, const bool* stop) {
  const ScopedOpenClose zip{*this};
  if (!zip) return false;

  FileStatSlim fileStat = {};
  if (!loadFileStatSlim(filename, &fileStat)) return false;

  const long fileOffset = getDataOffset(fileStat);
  if (fileOffset < 0) return false;

  file.seek(fileOffset);
  const auto deflatedDataSize = fileStat.compressedSize;
  const auto inflatedDataSize = fileStat.uncompressedSize;

  if (fileStat.method == ZIP_METHOD_STORED) {
    // no deflation, just read content
    const auto buffer = static_cast<uint8_t*>(malloc(chunkSize));
    if (!buffer) {
      LOG_ERR("ZIP", "Failed to allocate memory for buffer");
      return false;
    }

    size_t remaining = inflatedDataSize;
    while (remaining > 0) {
      // HalFile::read() reports an error as -1. Stored straight into a size_t it became SIZE_MAX,
      // passed the `== 0` check and went to the sink as the length of a 4 GB write out of this
      // chunk buffer. Ported from crosspoint-reader PR #3979 (phucisstupid).
      const int readResult = file.read(buffer, remaining < chunkSize ? remaining : chunkSize);
      if (readResult <= 0) {
        LOG_ERR("ZIP", "Could not read more bytes");
        free(buffer);
        return false;
      }

      const size_t dataRead = static_cast<size_t>(readResult);
      if (out.write(buffer, dataRead) != dataRead) {
        LOG_ERR("ZIP", "Failed to write all output bytes to stream");
        free(buffer);
        return false;
      }
      remaining -= dataRead;
      if (stop && *stop) break;
    }

    free(buffer);
    return true;
  }

  if (fileStat.method == ZIP_METHOD_DEFLATED) {
    auto* fileReadBuffer = static_cast<uint8_t*>(malloc(chunkSize));
    if (!fileReadBuffer) {
      LOG_ERR("ZIP", "Failed to allocate memory for zip file read buffer");
      return false;
    }

    auto* outputBuffer = static_cast<uint8_t*>(malloc(chunkSize));
    if (!outputBuffer) {
      LOG_ERR("ZIP", "Failed to allocate memory for output buffer");
      free(fileReadBuffer);
      return false;
    }

    ZipInflateCtx ctx;
    ctx.file = &file;
    ctx.fileRemaining = deflatedDataSize;
    ctx.readBuf = fileReadBuffer;
    ctx.readBufSize = chunkSize;

    // Ring sized to the entry (≤32 KB) — frees up to ~29 KB during small-entry streams.
    if (!ctx.reader.init(true, static_cast<size_t>(inflatedDataSize))) {
      LOG_ERR("ZIP", "Failed to init inflate reader");
      free(outputBuffer);
      free(fileReadBuffer);
      return false;
    }
    ctx.reader.setReadCallback(zipReadCallback);

    bool success = false;
    size_t totalProduced = 0;

    while (true) {
      size_t produced;
      const InflateStatus status = ctx.reader.readAtMost(outputBuffer, chunkSize, &produced);

      totalProduced += produced;
      if (totalProduced > static_cast<size_t>(inflatedDataSize)) {
        LOG_ERR("ZIP", "Decompressed size exceeds expected (%zu > %zu)", totalProduced,
                static_cast<size_t>(inflatedDataSize));
        break;
      }

      if (produced > 0) {
        if (out.write(outputBuffer, produced) != produced) {
          LOG_ERR("ZIP", "Failed to write all output bytes to stream");
          break;
        }
        if (stop && *stop) {
          success = true;
          break;
        }
      }

      if (status == InflateStatus::Done) {
        if (totalProduced != static_cast<size_t>(inflatedDataSize)) {
          LOG_ERR("ZIP", "Decompressed size mismatch (expected %zu, got %zu)", static_cast<size_t>(inflatedDataSize),
                  totalProduced);
          break;
        }
        LOG_DBG("ZIP", "Decompressed %d bytes into %d bytes", deflatedDataSize, inflatedDataSize);
        success = true;
        break;
      }

      if (status == InflateStatus::Error) {
        LOG_ERR("ZIP", "Decompression failed");
        break;
      }
      // InflateStatus::Ok: output buffer full, continue
    }

    free(outputBuffer);
    free(fileReadBuffer);
    return success;  // ctx.reader destructor frees the ring buffer
  }

  LOG_ERR("ZIP", "Unsupported compression method");
  return false;
}

size_t ZipFile::readBytesFromEntry(const char* filename, uint8_t* outBuf, const size_t maxBytes) {
  if (!outBuf || maxBytes == 0) return 0;

  const ScopedOpenClose zip{*this};
  if (!zip) return 0;

  FileStatSlim fileStat = {};
  if (!loadFileStatSlim(filename, &fileStat)) return 0;

  return readBytesFromStat(fileStat, outBuf, maxBytes);
}

size_t ZipFile::readBytesFromStat(const FileStatSlim& fileStat, uint8_t* outBuf, const size_t maxBytes,
                                  BuildArena* scratch) {
  if (!outBuf || maxBytes == 0) return 0;

  const ScopedOpenClose zip{*this};
  if (!zip) return 0;

  const long fileOffset = getDataOffset(fileStat);
  if (fileOffset < 0) return 0;

  file.seek(fileOffset);
  const size_t wantBytes = std::min(maxBytes, static_cast<size_t>(fileStat.uncompressedSize));

  if (fileStat.method == ZIP_METHOD_STORED) {
    const int n = file.read(outBuf, wantBytes);
    return n > 0 ? static_cast<size_t>(n) : 0;
  }

  if (fileStat.method == ZIP_METHOD_DEFLATED) {
    // Streaming inflate (32KB ring buffer) is the preferred path.  It can fail
    // if a 32KB ring buffer can't be allocated while another inflate is live
    // (e.g. reading image headers mid-XHTML stream).  In that case fall back to
    // one-shot inflate: read a bounded compressed chunk and decompress it all at
    // once without a ring buffer.
    constexpr size_t READ_BUF = 512;
    // The ring and read buffer from the caller's arena when it has room (the section build's lent
    // region: memory audit 2026-09, build inventory -- this probe and its caller's header buffer
    // were the build's heap peak, 8.7 KB inside one SAX chunk). A block of its own, released below
    // on every path, so the arena is left exactly as found.
    const size_t ringSize = InflateReader::ringSizeFor(wantBytes);
    BuildArena::Block scratchBlock;
    uint8_t* readBuf = nullptr;
    uint8_t* arenaRing = nullptr;
    if (scratch != nullptr && scratch->valid()) {
      scratchBlock = scratch->reserveBlock();
      readBuf = static_cast<uint8_t*>(scratch->alloc(READ_BUF));
      arenaRing = readBuf ? static_cast<uint8_t*>(scratch->alloc(ringSize)) : nullptr;
      if (arenaRing == nullptr) {
        scratch->release(scratchBlock);
        readBuf = nullptr;
      }
    }
    const bool inArena = arenaRing != nullptr;
    if (!inArena) readBuf = static_cast<uint8_t*>(malloc(READ_BUF));
    if (!readBuf) return 0;
    const auto releaseScratch = [&] {
      if (inArena) {
        scratch->release(scratchBlock);
      } else {
        free(readBuf);
      }
    };

    ZipInflateCtx ctx;
    ctx.file = &file;
    ctx.fileRemaining = fileStat.compressedSize;
    ctx.readBuf = readBuf;
    ctx.readBufSize = READ_BUF;

    // Size the ring to what the caller actually wants, not to the 32 KB default.
    //
    // init(true) with no size means ringSizeFor(0) -> INFLATE_DICT_SIZE, a full 32 KB ring for
    // a call that typically reads a 4 KB image header. ringSizeFor's contract is "total output
    // <= ring size guarantees no back-reference can outrun the ring", and wantBytes IS the
    // total output here — the loop below stops at wantBytes — so a ring of that size is exactly
    // as correct and up to 8x smaller.
    //
    // Measured cost of the old behaviour (X4 spine 1, per-page heap trace): free 51088 -> 6996
    // and contig 42996 -> 4084 between two page emissions, recovering pages later. That is this
    // ring, taken and released per image resolve, mid-parse — the single worst contiguous dip
    // in a section build, and it lands exactly where a PNG decode would want its own 32 KB.
    if (inArena ? ctx.reader.initWithExternalRing(arenaRing, ringSize) : ctx.reader.init(true, wantBytes)) {
      ctx.reader.setReadCallback(zipReadCallback);

      size_t totalOut = 0;
      while (totalOut < wantBytes) {
        size_t produced;
        const size_t remaining = wantBytes - totalOut;
        const InflateStatus status = ctx.reader.readAtMost(outBuf + totalOut, remaining, &produced);
        totalOut += produced;
        if (status == InflateStatus::Done || status == InflateStatus::Error) break;
      }

      ctx.reader.deinit();  // drops the ring pointer before its arena block goes back
      releaseScratch();
      return totalOut;
    }

    // Streaming ring buffer unavailable — fall back to one-shot inflate.
    // Read a bounded compressed chunk (4× the desired output as a rough overhead
    // estimate, capped at the actual compressed size) and decompress in one shot.
    ctx.reader.deinit();
    releaseScratch();
    const size_t compChunkSize = std::min(static_cast<size_t>(fileStat.compressedSize), wantBytes * 4);
    auto* compBuf = static_cast<uint8_t*>(malloc(compChunkSize));
    if (!compBuf) return 0;

    // Same -1-as-a-length hazard as readFileToStream: SIZE_MAX here would have uzlib inflate far
    // past the end of compBuf.
    const int compRead = file.read(compBuf, compChunkSize);
    if (compRead <= 0) {
      free(compBuf);
      return 0;
    }

    InflateReader oneShot;
    oneShot.init(false);
    oneShot.setSource(compBuf, static_cast<size_t>(compRead));

    // One-shot read: decompress up to wantBytes; partial output is still useful
    // (e.g. a JPEG SOF marker within the first kHeaderBufSize bytes).
    size_t produced = 0;
    const InflateStatus status = oneShot.readAtMost(outBuf, wantBytes, &produced);
    free(compBuf);
    if (status == InflateStatus::Error && produced == 0) return 0;
    return produced;
  }

  return 0;
}

// ---------------------------------------------------------------------------
// ZipFile::EntryReader — resumable per-entry inflate
// ---------------------------------------------------------------------------

struct ZipFile::EntryReader::Impl {
  ZipFile& zf;
  size_t chunkSize;
  // Optional arena backing (see EntryReader ctor doc): readBuf + inflate ring
  // are carved from one block reserved at open(), released in reset().
  BuildArena* arena = nullptr;
  BuildArena::Block arenaBlock;

  uint16_t method = 0;  // ZIP_METHOD_STORED or ZIP_METHOD_DEFLATED
  FsFile file;
  size_t inflatedSize_ = 0;
  size_t bytesProduced_ = 0;
  size_t outputCap_ = 0;  // 0 = the whole entry
  bool error_ = false;
  bool done_ = false;

  // Stored-entry tracking
  size_t storedRemaining = 0;

  // Deflated-entry state. ZipInflateCtx.reader must remain first (callback cast).
  ZipInflateCtx ctx = {};
  uint8_t* readBuf = nullptr;

  explicit Impl(ZipFile& zf_, size_t chunkSize_, BuildArena* arena_) : zf(zf_), chunkSize(chunkSize_), arena(arena_) {}
  ~Impl() { reset(); }
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  void reset() {
    if (readBuf) {
      ctx.reader.deinit();  // external ring: deinit only clears state, no free
      if (!arena) free(readBuf);
      readBuf = nullptr;
    }
    if (arena && arenaBlock.valid()) {
      arena->release(arenaBlock);
    }
    ctx.file = nullptr;
    ctx.fileRemaining = 0;
    ctx.readBuf = nullptr;
    ctx.readBufSize = 0;
    if (file) file.close();
    inflatedSize_ = 0;
    bytesProduced_ = 0;
    outputCap_ = 0;
    storedRemaining = 0;
    method = 0;
    error_ = false;
    done_ = false;
  }
};

// Nothrow: Impl carries the inflater's decode trees (~1.3 KB), and callers construct the reader
// with new (std::nothrow) / makeUniqueNoThrow precisely so a low heap fails the read rather than
// aborting the device -- a throwing allocation in here would undo that. Without it every call
// below fails like a missing entry.
ZipFile::EntryReader::EntryReader(ZipFile& zf, const size_t chunkSize, BuildArena* arena)
    : impl_(makeUniqueNoThrow<Impl>(zf, chunkSize, arena)) {
  if (!impl_) {
    LOG_ERR("ZIP", "EntryReader: out of memory for the reader state");
  }
}

ZipFile::EntryReader::~EntryReader() = default;
ZipFile::EntryReader::EntryReader(EntryReader&&) noexcept = default;
ZipFile::EntryReader& ZipFile::EntryReader::operator=(EntryReader&&) noexcept = default;

bool ZipFile::EntryReader::open(const char* filename) {
  if (!impl_) return false;
  FileStatSlim fileStat = {};
  if (!impl_->zf.loadFileStatSlim(filename, &fileStat)) {
    LOG_ERR("ZIP", "EntryReader::open: entry not found: %s", filename);
    return false;
  }
  return open(fileStat);
}

bool ZipFile::EntryReader::open(const FileStatSlim& fileStat) { return open(fileStat, 0); }

bool ZipFile::EntryReader::open(const FileStatSlim& fileStat, const size_t outputCap) {
  if (!impl_) return false;
  impl_->reset();
  impl_->outputCap_ = outputCap;
  // What the ring must cover: the whole entry, or only the prefix the caller will read.
  const size_t expectedOutput =
      outputCap == 0 ? fileStat.uncompressedSize : std::min<size_t>(fileStat.uncompressedSize, outputCap);

  const long dataOffset = impl_->zf.getDataOffset(fileStat);
  if (dataOffset < 0) {
    LOG_ERR("ZIP", "EntryReader::open: bad data offset (localHeaderOffset=%lu)",
            static_cast<unsigned long>(fileStat.localHeaderOffset));
    return false;
  }

  impl_->inflatedSize_ = fileStat.uncompressedSize;
  impl_->method = fileStat.method;

  if (!Storage.openFileForRead("ZIP", impl_->zf.filePath, impl_->file)) {
    LOG_ERR("ZIP", "EntryReader::open: failed to open zip file");
    return false;
  }
  impl_->file.seek(static_cast<size_t>(dataOffset));

  if (fileStat.method == ZIP_METHOD_STORED) {
    impl_->storedRemaining = fileStat.uncompressedSize;
    return true;
  }

  if (fileStat.method == ZIP_METHOD_DEFLATED) {
    if (impl_->arena) impl_->arenaBlock = impl_->arena->reserveBlock();
    impl_->readBuf = impl_->arena ? static_cast<uint8_t*>(impl_->arena->alloc(impl_->chunkSize))
                                  : static_cast<uint8_t*>(malloc(impl_->chunkSize));
    if (!impl_->readBuf) {
      LOG_ERR("ZIP", "EntryReader::open: OOM allocating read buffer");
      impl_->file.close();
      return false;
    }
    impl_->ctx.file = &impl_->file;
    impl_->ctx.fileRemaining = fileStat.compressedSize;
    impl_->ctx.readBuf = impl_->readBuf;
    impl_->ctx.readBufSize = impl_->chunkSize;
    // Ring sized to the entry (≤32 KB): small chapters cost a small ring, which is what
    // makes holding the reader across background-build slices affordable.
    bool ringOk;
    if (impl_->arena) {
      const size_t ringSize = InflateReader::ringSizeFor(expectedOutput);
      auto* ring = static_cast<uint8_t*>(impl_->arena->alloc(ringSize));
      ringOk = ring && impl_->ctx.reader.initWithExternalRing(ring, ringSize);
    } else {
      ringOk = impl_->ctx.reader.init(true, expectedOutput);
    }
    if (!ringOk) {
      LOG_ERR("ZIP", "EntryReader::open: OOM initialising inflate ring buffer");
      if (!impl_->arena) free(impl_->readBuf);
      impl_->readBuf = nullptr;
      if (impl_->arena && impl_->arenaBlock.valid()) {
        impl_->arena->release(impl_->arenaBlock);
      }
      impl_->file.close();
      return false;
    }
    impl_->ctx.reader.setReadCallback(zipReadCallback);
    return true;
  }

  LOG_ERR("ZIP", "EntryReader::open: unsupported compression method %u", fileStat.method);
  impl_->file.close();
  return false;
}

bool ZipFile::EntryReader::step(uint8_t* out, size_t cap, size_t* produced, bool* done) {
  *produced = 0;
  *done = false;

  if (!impl_) return false;
  if (impl_->done_) {
    *done = true;
    return true;
  }
  if (impl_->error_) return false;

  // Past the cap the ring no longer guarantees correct output, so the entry ends here.
  if (impl_->outputCap_ != 0) {
    const size_t remaining = impl_->outputCap_ - std::min(impl_->outputCap_, impl_->bytesProduced_);
    if (remaining == 0) {
      impl_->done_ = true;
      *done = true;
      return true;
    }
    if (cap > remaining) cap = remaining;
  }

  if (impl_->method == ZIP_METHOD_STORED) {
    if (impl_->storedRemaining == 0) {
      impl_->done_ = true;
      *done = true;
      return true;
    }
    const size_t toRead = cap < impl_->storedRemaining ? cap : impl_->storedRemaining;
    const int n = impl_->file.read(out, toRead);
    if (n <= 0) {
      LOG_ERR("ZIP", "EntryReader::step: stored read error");
      impl_->error_ = true;
      return false;
    }
    impl_->storedRemaining -= static_cast<size_t>(n);
    impl_->bytesProduced_ += static_cast<size_t>(n);
    *produced = static_cast<size_t>(n);
    if (impl_->storedRemaining == 0 || (impl_->outputCap_ != 0 && impl_->bytesProduced_ >= impl_->outputCap_)) {
      impl_->done_ = true;
      *done = true;
    }
    return true;
  }

  if (impl_->method == ZIP_METHOD_DEFLATED) {
    size_t p = 0;
    const InflateStatus status = impl_->ctx.reader.readAtMost(out, cap, &p);
    impl_->bytesProduced_ += p;
    *produced = p;
    if (status == InflateStatus::Done || (impl_->outputCap_ != 0 && impl_->bytesProduced_ >= impl_->outputCap_)) {
      impl_->done_ = true;
      *done = true;
      return true;
    }
    if (status == InflateStatus::Error) {
      LOG_ERR("ZIP", "EntryReader::step: inflate error");
      impl_->error_ = true;
      return false;
    }
    return true;
  }

  impl_->error_ = true;
  return false;
}

void ZipFile::EntryReader::close() {
  if (impl_) impl_->reset();
}
bool ZipFile::EntryReader::isOpen() const { return impl_ && static_cast<bool>(impl_->file); }
size_t ZipFile::EntryReader::inflatedSize() const { return impl_ ? impl_->inflatedSize_ : 0; }
size_t ZipFile::EntryReader::bytesProduced() const { return impl_ ? impl_->bytesProduced_ : 0; }
