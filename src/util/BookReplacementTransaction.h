#pragma once

#include <cstdint>

enum class BookReplacementDisposition : uint8_t { NoState, SameSource, Quarantine, Error };

enum class NonEpubReplacementRecovery : uint8_t {
  Nothing,
  CancelPending,
  RestoreOld,
  FinalizeReplacement,
  RemoveOldBackup,
  FailClosed,
};

constexpr NonEpubReplacementRecovery decideNonEpubReplacementRecovery(const bool pending, const bool hadBook,
                                                                      const bool finalExists,
                                                                      const bool oldBackupExists) {
  if (!pending) {
    // A backup-shaped sibling is not ours unless the durable pending marker
    // that predates its creation is still present. Preserve a marker-less
    // collision instead of deleting or restoring arbitrary user bytes.
    if (oldBackupExists) return NonEpubReplacementRecovery::FailClosed;
    return NonEpubReplacementRecovery::Nothing;
  }
  if (hadBook) {
    if (finalExists && !oldBackupExists) return NonEpubReplacementRecovery::CancelPending;
    if (!finalExists && oldBackupExists) return NonEpubReplacementRecovery::RestoreOld;
    if (finalExists && oldBackupExists) return NonEpubReplacementRecovery::FinalizeReplacement;
    return NonEpubReplacementRecovery::FailClosed;
  }
  if (!finalExists && !oldBackupExists) return NonEpubReplacementRecovery::CancelPending;
  if (finalExists && !oldBackupExists) return NonEpubReplacementRecovery::FinalizeReplacement;
  return NonEpubReplacementRecovery::FailClosed;
}

// Small policy seam for host fault tests. The canonical cache callback is
// intentionally last: once it renames the source-identity barrier, no later
// operation is allowed to fail.
template <typename QuarantineClippings, typename QuarantineBookmark, typename CommitCache>
bool runBookReplacementTransaction(const BookReplacementDisposition disposition,
                                   QuarantineClippings&& quarantineClippings, QuarantineBookmark&& quarantineBookmark,
                                   CommitCache&& commitCache) {
  if (disposition == BookReplacementDisposition::NoState || disposition == BookReplacementDisposition::SameSource) {
    return true;
  }
  if (disposition != BookReplacementDisposition::Quarantine) return false;
  return quarantineClippings() && quarantineBookmark() && commitCache();
}
