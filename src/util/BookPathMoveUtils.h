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

enum class BookFilePublishResult : uint8_t {
  Published,
  Unchanged,
  InvalidStagedFile,
  StateUnavailable,
  StorageError,
};

// Renames a book while migrating path-keyed progress, reader settings,
// statistics, bookmarks, and EPUB/TXT clippings. A failure before the file rename
// leaves the source authoritative; failures after it are rolled back when
// possible or leave verified recovery state for the destination reader.
BookPathMoveResult moveBookFilePreservingUserState(const std::string& sourcePath, const std::string& destinationPath);

// Atomically promotes a fully-written staging file to bookPath. EPUB state is
// protected by a durable fail-closed marker before the authoritative path is
// changed. On pre-publication failure the old book stays authoritative; after
// publication, failure is reported and old state remains quarantined/retryable.
BookFilePublishResult publishStagedBookFile(const std::string& stagingPath, const std::string& bookPath);

// Returns a same-directory hidden sibling suitable for an upload/download
// staging file. suffix must include its leading dot.
std::string hiddenBookFileSibling(const std::string& bookPath, const char* suffix);

// Repairs the small power-loss windows around staging -> final publication.
// Reader calls this before checking whether an EPUB's final path exists.
bool recoverInterruptedBookFileReplacement(const std::string& bookPath);

// Transaction files are internal even when the user enables ordinary hidden
// files; browsers and book scanners must never expose them for mutation.
bool isBookFileTransactionArtifact(const char* fileName);

// Must be checked before physically deleting a book whose reader tracks
// completion. Move/replace helpers also enforce this internally. A pending
// completion transaction keeps its exact cache path authoritative until
// recovery succeeds.
bool canDeleteOrRelocateBookFile(const std::string& bookPath);

// Call only after the backing file has been successfully deleted or replaced.
// Removes path-keyed data so a future unrelated book at the same path cannot
// inherit it. Cleanup is best-effort and never removes another path's state.
bool removeBookUserStateAfterDelete(const std::string& bookPath);

// Use after a successful upload/download has replaced the backing file. This
// also shadows an ambiguous pre-CrossVi bookmark fallback with an empty,
// canonical bookmark set so the new book cannot inherit stale bookmarks.
bool resetBookUserStateAfterReplacement(const std::string& bookPath);
