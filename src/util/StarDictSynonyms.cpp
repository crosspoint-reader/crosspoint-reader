#include "StarDictSynonyms.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <climits>

#include "StringUtils.h"

namespace StarDictSynonyms {
namespace {
constexpr uint32_t QSYN_MAGIC = 0x4E595351;  // "QSYN" little-endian
constexpr uint32_t QSYN_VERSION = 1;
constexpr size_t HEADER_BYTES = 5 * sizeof(uint32_t);
constexpr size_t SCAN_BYTES = 4096;
constexpr size_t MAX_ALIAS_BYTES = 255;

struct Header {
  uint32_t sampleCount = 0;
  uint32_t sourceSize = 0;
  bool valid = false;
};

Header readHeader(HalFile& file) {
  Header result;
  uint32_t raw[5];
  if (!file.seekSet(0) || file.read(raw, sizeof(raw)) != static_cast<int>(sizeof(raw))) return result;
  if (raw[0] != QSYN_MAGIC || raw[1] != QSYN_VERSION || raw[2] != SAMPLE_INTERVAL) return result;
  result.sampleCount = raw[3];
  result.sourceSize = raw[4];
  result.valid = true;
  return result;
}

bool headerMatchesSource(HalFile& file, const Header& header, const uint64_t sourceSize) {
  if (!header.valid || header.sourceSize != sourceSize) return false;
  const uint64_t expectedSize = HEADER_BYTES + static_cast<uint64_t>(header.sampleCount) * sizeof(uint32_t);
  return expectedSize <= UINT32_MAX && file.fileSize64() == expectedSize;
}

bool readSample(HalFile& file, uint32_t index, uint32_t& offset) {
  const uint64_t position = HEADER_BYTES + static_cast<uint64_t>(index) * sizeof(uint32_t);
  if (position > UINT32_MAX || !file.seekSet(static_cast<uint32_t>(position))) return false;
  return file.read(&offset, sizeof(offset)) == static_cast<int>(sizeof(offset));
}

uint32_t readBe32(const uint8_t bytes[4]) {
  return (static_cast<uint32_t>(bytes[0]) << 24) | (static_cast<uint32_t>(bytes[1]) << 16) |
         (static_cast<uint32_t>(bytes[2]) << 8) | static_cast<uint32_t>(bytes[3]);
}

bool readEntry(HalFile& file, char* alias, size_t aliasCapacity, uint32_t& ordinal) {
  size_t length = 0;
  while (true) {
    const int ch = file.read();
    if (ch < 0) return false;
    if (ch == 0) break;
    if (length + 1 >= aliasCapacity) return false;
    alias[length++] = static_cast<char>(ch);
  }
  if (length == 0) return false;
  alias[length] = '\0';
  uint8_t suffix[4];
  if (file.read(suffix, sizeof(suffix)) != static_cast<int>(sizeof(suffix))) return false;
  ordinal = readBe32(suffix);
  return true;
}

bool publishHeader(HalFile& output, uint32_t sampleCount, uint32_t sourceSize) {
  const uint32_t header[5] = {QSYN_MAGIC, QSYN_VERSION, SAMPLE_INTERVAL, sampleCount, sourceSize};
  return output.seekSet(0) && output.write(header, sizeof(header)) == sizeof(header) && output.sync() && output.close();
}
}  // namespace

bool needsIndex(const std::string& basePath) {
  const std::string sourcePath = basePath + ".syn";
  if (!Storage.exists(sourcePath.c_str())) return false;
  HalFile source;
  if (!Storage.openFileForRead("SYN", sourcePath, source)) return false;
  const uint64_t sourceSize = source.fileSize64();
  if (sourceSize > UINT32_MAX) return true;

  HalFile sidecar;
  if (!Storage.openFileForRead("SYN", basePath + ".qsyn", sidecar)) return true;
  const Header header = readHeader(sidecar);
  return !headerMatchesSource(sidecar, header, sourceSize);
}

bool buildIndex(const std::string& basePath, void (*yieldFn)(void*), void* ctx) {
  const std::string sourcePath = basePath + ".syn";
  if (!Storage.exists(sourcePath.c_str())) return true;

  HalFile source;
  if (!Storage.openFileForRead("SYN", sourcePath, source)) return false;
  const uint64_t sourceSize64 = source.fileSize64();
  if (sourceSize64 > UINT32_MAX) return false;
  const uint32_t sourceSize = static_cast<uint32_t>(sourceSize64);

  auto buffer = makeUniqueNoThrow<uint8_t[]>(SCAN_BYTES);
  if (!buffer) return false;

  const std::string sidecarPath = basePath + ".qsyn";
  HalFile output;
  if (!Storage.openFileForWrite("SYN", sidecarPath, output)) return false;
  const uint32_t placeholder[5] = {};
  if (output.write(placeholder, sizeof(placeholder)) != sizeof(placeholder)) return false;

  uint32_t sampleCount = 0;
  uint32_t entryCount = 0;
  uint32_t position = 0;
  uint32_t sinceYield = 0;
  size_t aliasLength = 0;
  uint8_t suffixLeft = 0;
  bool valid = true;

  if (sourceSize > 0) {
    const uint32_t firstOffset = 0;
    valid = output.write(&firstOffset, sizeof(firstOffset)) == sizeof(firstOffset);
    sampleCount = valid ? 1 : 0;
  }

  while (valid && position < sourceSize) {
    const int count = source.read(buffer.get(), SCAN_BYTES);
    if (count <= 0) {
      valid = false;
      break;
    }
    for (int index = 0; valid && index < count; ++index) {
      if (suffixLeft == 0) {
        if (buffer[index] == 0) {
          if (aliasLength == 0 || aliasLength > MAX_ALIAS_BYTES) {
            valid = false;
            break;
          }
          suffixLeft = 4;
        } else {
          ++aliasLength;
          if (aliasLength > MAX_ALIAS_BYTES) valid = false;
        }
      } else if (--suffixLeft == 0) {
        ++entryCount;
        aliasLength = 0;
        const uint32_t nextEntry = position + static_cast<uint32_t>(index) + 1;
        if (entryCount % SAMPLE_INTERVAL == 0 && nextEntry < sourceSize) {
          valid = output.write(&nextEntry, sizeof(nextEntry)) == sizeof(nextEntry);
          if (valid) ++sampleCount;
        }
      }
    }
    position += static_cast<uint32_t>(count);
    sinceYield += static_cast<uint32_t>(count);
    if (yieldFn && sinceYield >= 64 * 1024) {
      sinceYield = 0;
      yieldFn(ctx);
    }
  }
  valid = valid && aliasLength == 0 && suffixLeft == 0 && position == sourceSize;

  if (!valid) {
    LOG_ERR("SYN", "Malformed synonym file: %s", sourcePath.c_str());
    // Keep a valid zero-sample marker. Direct .idx lookups remain available,
    // and the same malformed file is not rescanned on every lookup.
    output.close();
    Storage.remove(sidecarPath.c_str());
    HalFile disabled;
    if (!Storage.openFileForWrite("SYN", sidecarPath, disabled) || !publishHeader(disabled, 0, sourceSize)) {
      Storage.remove(sidecarPath.c_str());
    }
    return false;
  }
  if (!publishHeader(output, sampleCount, sourceSize)) {
    Storage.remove(sidecarPath.c_str());
    return false;
  }
  return true;
}

bool lookupOrdinal(const std::string& basePath, const char* target, uint32_t& ordinalOut) {
  if (!target || *target == '\0') return false;
  HalFile source;
  if (!Storage.openFileForRead("SYN", basePath + ".syn", source)) return false;
  const uint64_t sourceSize64 = source.fileSize64();
  if (sourceSize64 > UINT32_MAX) return false;

  HalFile sidecar;
  if (!Storage.openFileForRead("SYN", basePath + ".qsyn", sidecar)) return false;
  const Header header = readHeader(sidecar);
  if (!headerMatchesSource(sidecar, header, sourceSize64) || header.sampleCount == 0) return false;

  char alias[MAX_ALIAS_BYTES + 1];
  uint32_t lo = 0;
  uint32_t hi = header.sampleCount - 1;
  while (lo < hi) {
    const uint32_t middle = (lo + hi + 1) / 2;
    uint32_t offset = 0;
    uint32_t ignoredOrdinal = 0;
    if (!readSample(sidecar, middle, offset) || offset >= sourceSize64 || !source.seekSet(offset) ||
        !readEntry(source, alias, sizeof(alias), ignoredOrdinal)) {
      return false;
    }
    if (StringUtils::asciiCaseCmp(alias, target) <= 0) {
      lo = middle;
    } else {
      hi = middle - 1;
    }
  }

  uint32_t startOffset = 0;
  if (!readSample(sidecar, lo, startOffset) || !source.seekSet(startOffset)) return false;
  for (uint32_t count = 0; count < SAMPLE_INTERVAL && source.position() < sourceSize64; ++count) {
    uint32_t ordinal = 0;
    if (!readEntry(source, alias, sizeof(alias), ordinal)) return false;
    const int comparison = StringUtils::asciiCaseCmp(alias, target);
    if (comparison == 0) {
      ordinalOut = ordinal;
      return true;
    }
    if (comparison > 0) return false;
  }
  return false;
}
}  // namespace StarDictSynonyms
