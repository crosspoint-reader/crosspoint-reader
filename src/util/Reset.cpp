#include "Reset.h"

#include <HalStorage.h>
#include <Logging.h>

#include "CrossPointSettings.h"

bool resetCrossPointSettings() {
  if (!Storage.ready()) {
    LOG_ERR("RESET", "SD card is not ready");
    return false;
  }
  // Boot supplies current board defaults without changing credentials or reading data.
  if (!Storage.remove(CrossPointSettings::getFilePath())) {
    LOG_ERR("RESET", "Could not remove saved preferences");
    return false;
  }
  return true;
}

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
