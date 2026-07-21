#include <Logging.h>

#if __has_include(<AppVersion.h>)
#include <AppVersion.h>
#endif

#ifdef CROSSINK_VERSION
#include <atomic>
#endif

#include "network/OtaUpdater.h"

bool OtaUpdater::isUpdateNewer() const { return false; }
const std::string &OtaUpdater::getLatestVersion() const {
  static const std::string version = "dev-simulator";
  return version;
}

OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() {
  LOG_DBG("OTA", "[SIM] OTA check is non-destructive; reporting no update");
  return NO_UPDATE;
}

#ifdef CROSSINK_VERSION
OtaUpdater::OtaUpdaterError
OtaUpdater::installUpdate(ProgressCallback onProgress, void *ctx,
                          std::atomic<bool> *cancelRequested) {
  LOG_DBG("OTA", "[SIM] OTA install is not supported in the native simulator");
  processedSize = 1;
  totalSize = 1;
  if (onProgress)
    onProgress(ctx);
  if (cancelRequested && cancelRequested->load())
    return CANCELLED_ERROR;
  return INTERNAL_UPDATE_ERROR;
}
#else
OtaUpdater::OtaUpdaterError
OtaUpdater::installUpdate(ProgressCallback onProgress, void *ctx) {
  LOG_DBG("OTA", "[SIM] OTA install is not supported in the native simulator");
  processedSize = 1;
  totalSize = 1;
  if (onProgress)
    onProgress(ctx);
  return INTERNAL_UPDATE_ERROR;
}
#endif
