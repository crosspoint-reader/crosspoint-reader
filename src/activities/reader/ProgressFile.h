#pragma once

#include <HalStorage.h>
#include <Logging.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace ProgressFile {

// Writes `len` bytes of reader progress to `<cachePath>/progress.bin` without
// ever leaving the canonical file half-written.
//
// The bytes go to a temporary `progress.bin.tmp` first; only once that is fully
// written and closed is it renamed over progress.bin. An interrupted write
// (power loss or a crash mid-SPI) therefore damages only the throwaway temp file.
// Previously a truncate-in-place write that was cut short left progress.bin with
// a broken FAT cluster chain that the firmware could neither rewrite nor clear,
// stranding the book on an old page (issue #2275).
//
// This is crash-safe, not metadata-atomic: on FAT the replace is remove + rename,
// two separate directory operations, so a crash between them can leave neither
// file -- which simply reads as "no saved progress" on next launch, never a
// corrupt or unclearable file. The point is that progress.bin is never torn.
//
// Note: this prevents corruption on a healthy card going forward. It cannot
// repair an already-corrupted progress.bin -- removing the stale file may itself
// fail at the FAT level, in which case recovery still requires fsck on a host.
//
// Returns true only if the new progress.bin is fully in place.
inline bool writeAtomic(const std::string& cachePath, const uint8_t* data, size_t len) {
  const std::string finalPath = cachePath + "/progress.bin";
  const std::string tmpPath = cachePath + "/progress.bin.tmp";

  {
    HalFile f;
    if (!Storage.openFileForWrite("PRG", tmpPath, f)) {
      LOG_ERR("PRG", "Could not open temp progress file for write: %s", tmpPath.c_str());
      return false;
    }
    const size_t written = f.write(data, len);
    if (written != len) {
      LOG_ERR("PRG", "Short write saving progress to %s: %u/%u bytes", tmpPath.c_str(), (unsigned)written,
              (unsigned)len);
      return false;
    }
    f.flush();
    // f (the temp file) is closed at scope exit (DESTRUCTOR_CLOSES_FILE=1) before
    // the rename below -- SdFat must not rename a path that still has an open FsFile.
  }

  // SdFat's rename does not overwrite an existing destination, so drop the old
  // canonical file first. The brief window where neither file exists reads as
  // "no saved progress" on next launch -- never a corrupt, unclearable file.
  Storage.remove(finalPath.c_str());
  if (!Storage.rename(tmpPath.c_str(), finalPath.c_str())) {
    LOG_ERR("PRG", "Failed to rename temp progress into place: %s", finalPath.c_str());
    return false;
  }
  return true;
}

struct EpubProgress {
  uint32_t spineIndex = 0;
  uint32_t pageNumber = 0;
  uint32_t chapterTotalPages = 0;
  uint32_t visibleTextOffset = 0;
  bool hasVisibleTextOffset = false;
  bool success = false;
};

inline bool readEpubProgress(HalFile& file, EpubProgress& progress) {
  uint8_t data[10];
  int dataSize = file.read(data, sizeof(data));
  if (dataSize == 4 || dataSize == 6 || dataSize == 10) {
    progress.spineIndex = static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8);
    progress.pageNumber = static_cast<uint32_t>(data[2]) | (static_cast<uint32_t>(data[3]) << 8);
    if (progress.pageNumber == UINT16_MAX) {
      progress.pageNumber = 0;
    }
    if (dataSize == 6 || dataSize == 10) {
      progress.chapterTotalPages = static_cast<uint32_t>(data[4]) | (static_cast<uint32_t>(data[5]) << 8);
    }
    if (dataSize == 10) {
      progress.visibleTextOffset = static_cast<uint32_t>(data[6]) |
                                   (static_cast<uint32_t>(data[7]) << 8) |
                                   (static_cast<uint32_t>(data[8]) << 16) |
                                   (static_cast<uint32_t>(data[9]) << 24);
      progress.hasVisibleTextOffset = true;
    }
    progress.success = true;
    return true;
  }
  return false;
}

inline bool read32BitPageProgress(HalFile& file, uint32_t& page) {
  uint8_t data[4];
  if (file.read(data, 4) == 4) {
    page = static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
    return true;
  }
  return false;
}

}  // namespace ProgressFile
