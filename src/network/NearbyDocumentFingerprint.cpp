#include "NearbyDocumentFingerprint.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <MD5Builder.h>

#include <algorithm>
#include <array>

std::string calculateNearbyDocumentFingerprint(const std::string& path) {
  HalFile file;
  if (!Storage.openFileForRead("CVNEAR", path, file)) return {};
  const uint64_t expectedSize = file.fileSize64();
  if (expectedSize == 0) {
    file.close();
    return {};
  }

  MD5Builder md5;
  md5.begin();
  std::array<uint8_t, 2048> buffer{};
  uint64_t totalRead = 0;
  size_t bytesSinceYield = 0;
  while (totalRead < expectedSize) {
    const size_t wanted = static_cast<size_t>(std::min<uint64_t>(buffer.size(), expectedSize - totalRead));
    const int read = file.read(buffer.data(), wanted);
    if (read <= 0 || static_cast<size_t>(read) > wanted) {
      LOG_ERR("CVNEAR", "Could not fingerprint complete EPUB: %s", path.c_str());
      file.close();
      return {};
    }
    md5.add(buffer.data(), static_cast<size_t>(read));
    totalRead += static_cast<uint64_t>(read);
    bytesSinceYield += static_cast<size_t>(read);
    if (bytesSinceYield >= 64U * 1024U) {
      yield();
      bytesSinceYield = 0;
    }
  }
  // Refuse a concurrently replaced/appended file instead of treating a prefix
  // hash as an exact document identity.
  const bool sizeStayedStable = file.fileSize64() == expectedSize;
  if (totalRead != expectedSize || !sizeStayedStable || !file.close()) return {};
  md5.calculate();
  return md5.toString().c_str();
}
