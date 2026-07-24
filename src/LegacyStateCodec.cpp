#include "LegacyStateCodec.h"

#include <HalStorage.h>

#include <cstddef>
#include <utility>

namespace LegacyStateCodec {
namespace {

DecodeStatus readExact(HalFile& file, void* destination, const size_t size) {
  if (size == 0) return DecodeStatus::Ok;
  if (file.fileSize64() < file.position() || file.fileSize64() - file.position() < size) {
    return DecodeStatus::Invalid;
  }
  if (file.read(destination, size) != static_cast<int>(size)) {
    return file.getError() == 0 ? DecodeStatus::Invalid : DecodeStatus::IoError;
  }
  return DecodeStatus::Ok;
}

template <typename T>
DecodeStatus readValue(HalFile& file, T& value) {
  return readExact(file, &value, sizeof(value));
}

}  // namespace

DecodeStatus decode(HalFile& file, State& state) {
  State parsed;
  uint8_t version = 0;
  DecodeStatus status = readValue(file, version);
  if (status != DecodeStatus::Ok) return status;
  if (version > CURRENT_VERSION) return DecodeStatus::FutureVersion;

  uint32_t pathLength = 0;
  status = readValue(file, pathLength);
  if (status != DecodeStatus::Ok) return status;
  if (pathLength > MAX_BOOK_PATH_BYTES || file.fileSize64() < file.position() ||
      file.fileSize64() - file.position() < pathLength) {
    return DecodeStatus::Invalid;
  }
  parsed.openBookPath.resize(pathLength);
  status = readExact(file, pathLength == 0 ? nullptr : parsed.openBookPath.data(), pathLength);
  if (status != DecodeStatus::Ok) return status;

  if (version >= 2) {
    status = readValue(file, parsed.lastSleepImage);
    if (status != DecodeStatus::Ok) return status;
  }
  if (version >= 3) {
    status = readValue(file, parsed.readerActivityLoadCount);
    if (status != DecodeStatus::Ok) return status;
  }
  if (version >= 4) {
    status = readValue(file, parsed.lastSleepFromReader);
    if (status != DecodeStatus::Ok) return status;
  }

  state = std::move(parsed);
  return DecodeStatus::Ok;
}

}  // namespace LegacyStateCodec
