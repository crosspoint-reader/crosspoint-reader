#pragma once

#include <cstdint>
#include <string>

#include "BookReadingStats.h"
#include "GlobalReadingStats.h"

namespace ReadingStatsCompletionTransaction {

enum class RecoveryResult : uint8_t { NoMarker, Recovered, Blocked };

// Completes an interrupted per-book/global completion transition. The marker
// is bound to one exact cache path; a marker for another book is left alone.
RecoveryResult recover(const std::string& cachePath);

// Recovers the exact tracked-book cache path embedded in a strictly validated marker,
// so opening a different book cannot leave global statistics locked forever.
RecoveryResult recoverPending();

// Atomically coordinates the two independently crash-safe stats files. Only a
// completed <-> incomplete transition is accepted; all other global fields
// must remain byte-identical.
bool commit(const std::string& cachePath, const BookReadingStats& oldBookStats, const BookReadingStats& newBookStats,
            const GlobalReadingStats& oldGlobalStats, const GlobalReadingStats& newGlobalStats);

// Public stats writers use these guards so an unresolved transaction cannot
// be overwritten by an unrelated periodic save or reset.
bool permitsBookWrite(const std::string& cachePath, const BookReadingStats& stats);
// False while this exact tracked-book cache is named by a valid pending
// transaction; malformed markers block all tracked-book cache
// relocation/deletion/quarantine.
bool canRelocateOrDeleteBookCache(const std::string& cachePath);
bool permitsGlobalWrite(const GlobalReadingStats& stats);
// Global reset is never part of a completion transaction, even when its zero
// payload happens to match a transaction endpoint.
bool canResetGlobalStats();

}  // namespace ReadingStatsCompletionTransaction
