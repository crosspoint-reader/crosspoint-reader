#pragma once

#include <cstdint>
#include <string>

enum class BookPathMoveResult : uint8_t {
  Moved,
  SourceMissing,
  DestinationExists,
  StateUnavailable,
  StorageError,
};

// Renames a book while migrating path-keyed progress, reader settings,
// statistics, bookmarks, and EPUB clippings. A failure before the file rename
// leaves the source authoritative; failures after it are rolled back when
// possible or leave verified recovery state for the destination reader.
BookPathMoveResult moveBookFilePreservingUserState(const std::string& sourcePath, const std::string& destinationPath);

// Call only after the backing file has been successfully deleted or replaced.
// Removes path-keyed data so a future unrelated book at the same path cannot
// inherit it. Cleanup is best-effort and never removes another path's state.
void removeBookUserStateAfterDelete(const std::string& bookPath);

// Use after a successful upload/download has replaced the backing file. This
// also shadows an ambiguous pre-CrossVi bookmark fallback with an empty,
// canonical bookmark set so the new book cannot inherit stale bookmarks.
void resetBookUserStateAfterReplacement(const std::string& bookPath);
