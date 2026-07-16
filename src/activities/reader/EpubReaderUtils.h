#pragma once

#include <Epub.h>
#include <Logging.h>

#include <ctime>

#include "KOReaderCredentialStore.h"  // gate the BLE timestamp on the sync toggle
#include "ProgressFile.h"
#include "ble_sync/BleClock.h"  // advance the persisted clock floor on a real save

namespace EpubReaderUtils {

// Persists reader progress for an EPUB to its cache directory. Returns true on success.
inline bool saveProgress(const Epub& epub, int spineIndex, int pageNumber, int pageCount) {
  if (spineIndex < 0 || spineIndex > 0xFFFF || pageNumber < 0 || pageNumber > 0xFFFF || pageCount < 0 ||
      pageCount > 0xFFFF) {
    LOG_ERR("ERS", "Progress values out of range: spine=%d page=%d count=%d", spineIndex, pageNumber, pageCount);
    return false;
  }

  // Did the reading position actually change? Read the existing progress.bin
  // BEFORE overwriting it. This gates the BLE conflict clock below: merely
  // re-opening a book (which re-saves the SAME page) must NOT bump the timestamp,
  // or this device would perpetually look "newer" than the phone and clobber a
  // genuinely newer position the phone holds. Only real page turns advance it.
  bool positionChanged = true;
  if (KOREADER_STORE.getBleSyncEnabled()) {
    HalFile rf;
    if (Storage.openFileForRead("ERS", epub.getCachePath() + "/progress.bin", rf)) {
      uint8_t old[6] = {0};
      if (rf.read(old, sizeof(old)) >= 4) {
        const int oldSpine = old[0] | (old[1] << 8);
        const int oldPage = old[2] | (old[3] << 8);
        positionChanged = (oldSpine != spineIndex) || (oldPage != pageNumber);
      }
    }
  }

  uint8_t data[6];
  data[0] = spineIndex & 0xFF;
  data[1] = (spineIndex >> 8) & 0xFF;
  data[2] = pageNumber & 0xFF;
  data[3] = (pageNumber >> 8) & 0xFF;
  data[4] = pageCount & 0xFF;
  data[5] = (pageCount >> 8) & 0xFF;
  if (!ProgressFile::writeAtomic(epub.getCachePath(), data, sizeof(data))) {
    return false;
  }
  LOG_DBG("ERS", "Progress saved: spine=%d page=%d", spineIndex, pageNumber);

  // Per-book last-CHANGE timestamp for BLE sync conflict resolution (newest-wins).
  // ONLY when BLE sync is enabled AND the position actually changed — zero extra
  // work when the feature is off, and no spurious "newer" bump on a plain re-open.
  // Non-atomic; a torn write only yields a slightly-off time, never corruption.
  //
  // Clock not set yet (before the first BLE NTP of this boot): stamp an explicit
  // 0 instead of keeping the old value. A stale stamp makes this fresh position
  // lose newest-wins forever (the phone then overwrites it); the 0 marker gets
  // backfilled with the real time when the phone's clock arrives
  // (BleProgress::backfillUnclockedTimestamps).
  if (positionChanged && KOREADER_STORE.getBleSyncEnabled()) {
    const time_t now = time(nullptr);
    uint64_t stamp = 0;
    if (now > 1000000000) {
      // STRICTLY MONOTONIC per book: a page turn must out-rank the last synced
      // value even within the same wall-clock second. Otherwise an edit made in
      // the same second as the previous sync ties the peer's timestamp and
      // neither side pushes (the equal-timestamp stall). Read the prior stamp
      // and take max(now, prior+1).
      int64_t prior = 0;
      HalFile rt;
      if (Storage.openFileForRead("ERS", epub.getCachePath() + "/progress-time.bin", rt)) {
        uint8_t od[8] = {0};
        if (rt.read(od, sizeof(od)) >= 8) {
          for (int i = 0; i < 8; i++) prior |= (static_cast<int64_t>(od[i]) << (8 * i));
        }
      }
      const int64_t base = (static_cast<int64_t>(now) > prior + 1) ? static_cast<int64_t>(now) : prior + 1;
      stamp = static_cast<uint64_t>(base);
    }
    uint8_t tb[8];
    for (int i = 0; i < 8; i++) tb[i] = static_cast<uint8_t>((stamp >> (8 * i)) & 0xFF);
    HalFile tf;
    if (Storage.openFileForWrite("ERS", epub.getCachePath() + "/progress-time.bin", tf)) {
      tf.write(tb, sizeof(tb));
      tf.flush();
    }
    // Advance the persisted clock floor so offline reading stays monotonic
    // across the daily deep-sleep reset (PROTOCOL-v3 §3). No-op if unclocked.
    BleClock::writeFloor(static_cast<int64_t>(stamp));
  }
  return true;
}

}  // namespace EpubReaderUtils
