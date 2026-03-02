#pragma once

#include <string>

class GfxRenderer;

class FirmwareUpdateUtil {
 public:
  static constexpr const char* kFirmwareBinPath = "/firmware.bin";

  /**
   * Checks if a firmware update file exists on the SD card.
   * @return true if /firmware.bin exists.
   */
  static bool checkForLocalUpdate();

  /**
   * Performs the firmware update from /firmware.bin.
   * Displays progress using the provided renderer.
   * @param renderer The renderer to use for progress display.
   * @return true if the update was successful (the device will reboot on success).
   */
  static bool performLocalUpdate(GfxRenderer& renderer);
};
