#pragma once

#include <string>

// Clears the reading cache for a book file if its extension is recognised
// (EPUB, XTC, or TXT). Does nothing for other file types.
void clearBookCache(const std::string& path);

// Clears derived files from a known book cache directory while preserving
// per-book statistics and CrossVi reader settings. Interrupted clears are
// recovered from a sibling staging directory on the next call.
bool clearBookCacheDirectoryPreservingUserState(const std::string& cachePath);

// Restores user-state files left in the sibling staging directory by an
// interrupted cache clear, without deleting any derived cache data. Call this
// before loading or creating per-book state.
bool recoverBookCacheUserState(const std::string& cachePath, const std::string& bookPath);

// After the backing book has been deleted or replaced, atomically moves every
// recognised cache/staging transaction for this path out of the recovery
// namespace before best-effort deletion. Unknown or mismatched transaction
// data is preserved and reported as a failure so Reader recovery fails closed.
bool resetBookCacheUserStateAfterReplacement(const std::string& cachePath, const std::string& bookPath);

// Crash-tolerant state migration for a book whose path is about to change.
// prepare copies only non-rebuildable state to a verified sibling staging
// directory while leaving the source authoritative. finalize publishes that
// staging directory after the book rename; recoverBookCacheUserState() can
// finish the publication after a reset.
bool prepareBookCacheUserStateMove(const std::string& sourceCachePath, const std::string& destinationCachePath,
                                   const std::string& sourceBookPath, const std::string& destinationBookPath);
bool finalizeBookCacheUserStateMove(const std::string& sourceCachePath, const std::string& destinationCachePath,
                                    const std::string& sourceBookPath, const std::string& destinationBookPath);
bool cancelBookCacheUserStateMove(const std::string& sourceCachePath, const std::string& destinationCachePath,
                                  const std::string& sourceBookPath, const std::string& destinationBookPath);
bool discardPublishedBookCacheUserStateMove(const std::string& sourceCachePath, const std::string& destinationCachePath,
                                            const std::string& sourceBookPath, const std::string& destinationBookPath);
// Removes only the old bookmark copy after the destination book and verified
// bookmark are authoritative. Failure is non-destructive and leaves a duplicate.
bool completeBookCacheUserStateMove(const std::string& sourceCachePath, const std::string& destinationCachePath,
                                    const std::string& sourceBookPath, const std::string& destinationBookPath);

// Returns true if the directory name matches a book cache entry.
bool isBookCacheDirectoryName(const char* name);
