#include <Logging.h>

#include "network/FirmwareFlasher.h"
#include "network/OtaBootSwitch.h"

namespace firmware_flash {
Result flashFromSdPath(const char *, ProgressCb onProgress, void *ctx, bool) {
  LOG_DBG("FLASH",
          "[SIM] Firmware flashing is not supported in the native simulator");
  if (onProgress)
    onProgress(1, 1, ctx);
  return Result::WRITE_FAIL;
}

Result validateImageFile(const char *, size_t) {
  LOG_DBG(
      "FLASH",
      "[SIM] Firmware image validation is disabled in the native simulator");
  return Result::WRITE_FAIL;
}

const char *resultName(Result r) {
  switch (r) {
  case Result::OK:
    return "OK";
  case Result::OPEN_FAIL:
    return "OPEN_FAIL";
  case Result::TOO_SMALL:
    return "TOO_SMALL";
  case Result::TOO_LARGE:
    return "TOO_LARGE";
  case Result::BAD_MAGIC:
    return "BAD_MAGIC";
  case Result::BAD_SEGMENTS:
    return "BAD_SEGMENTS";
  case Result::BAD_CHECKSUM:
    return "BAD_CHECKSUM";
  case Result::BAD_SHA:
    return "BAD_SHA";
  case Result::BAD_SIZE:
    return "BAD_SIZE";
  case Result::NO_PARTITION:
    return "NO_PARTITION";
  case Result::OOM:
    return "OOM";
  case Result::READ_FAIL:
    return "READ_FAIL";
  case Result::ERASE_FAIL:
    return "ERASE_FAIL";
  case Result::WRITE_FAIL:
    return "UNSUPPORTED_IN_SIMULATOR";
  case Result::OTADATA_FAIL:
    return "OTADATA_FAIL";
  default:
    return "UNKNOWN";
  }
}
} // namespace firmware_flash

namespace ota_boot {
uint32_t computeSeqCrc(uint32_t) { return 0; }
bool switchTo(const esp_partition_t *) {
  LOG_DBG("FLASH", "[SIM] Boot partition switching is not supported in the "
                   "native simulator");
  return false;
}
} // namespace ota_boot
