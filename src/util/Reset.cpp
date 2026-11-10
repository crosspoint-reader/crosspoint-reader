#include "Reset.h"

#include <HalStorage.h>
#include <Logging.h>

bool resetCrossPointData() {
  if (!Storage.ready()) {
    LOG_ERR("RESET", "SD card is not ready");
    return false;
  }
  // Preserve books, installed fonts/dictionaries, and device keys/calibration in NVS.
  if (!Storage.removeDir("/.crosspoint")) {
    LOG_ERR("RESET", "Could not erase CrossPoint data; reset may be incomplete");
    return false;
  }
  return true;
}
