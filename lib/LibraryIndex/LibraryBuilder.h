#pragma once

// Builds the CLX1 index by walking the SD card once.
//
// M1 derives everything from paths and filenames; reading titles and authors out
// of the EPUBs themselves is M2 (LibraryEnrich), which rewrites records in place
// and never re-walks.
//
// Shape of the build, and why:
//
//   * ONE walk. Records go straight into a staging file in discovery order, so
//     nothing proportional to the library stays resident. Only a small sort array
//     does, and it is capped.
//   * Duplicate directory entries are dropped. A damaged FAT can hand the same
//     file out twice — measured on a real card: 6 of 75 entries were duplicate
//     dirents resolving to one inode — and without this the shelf shows phantom
//     books that cannot be opened.
//   * Unreadable entries are skipped, never fatal. The same card had 7 entries
//     whose names enumerate but whose contents cannot be opened.
//   * Install is write-then-rename, so an interrupted build leaves the previous
//     index untouched rather than a half-written one.

#include <cstdint>
#include <string>

#include "LibraryFormat.h"

namespace library {

// Directory levels below the scan root that are walked. The measured corpus is
// two deep (genre/author/book); the cap exists because a corrupted FAT can
// contain a directory that contains itself — also measured on the same card —
// and an uncapped walk would never return.
inline constexpr int LIBRARY_MAX_DEPTH = 5;

// Books held in the in-RAM sort array. 14 bytes each, so this is 7 KB — one
// bounded allocation, in the band the codebase allows without a heap gate.
// Beyond it the index is still built and still complete, but in walk order with
// CLIX_FLAG_RANKS_DEGRADED set, which the screen reports rather than hides.
inline constexpr uint16_t LIBRARY_MAX_SORTED = 512;

// Duplicate identities remembered while one directory is enumerated. The
// fixed, fallible allocation is 8 KiB at this cap; unlike std::vector it cannot
// grow into abort() when a damaged or unusually flat directory is scanned.
inline constexpr uint16_t LIBRARY_MAX_DEDUP_KEYS = 1024;

struct BuildStats {
  uint16_t books = 0;
  uint16_t folders = 0;
  uint16_t duplicatesDropped = 0;
  uint16_t unreadableSkipped = 0;
  uint32_t walkMs = 0;
  // Reconciliation against the previous index. Their sum over a rebuild with no
  // card changes should be: unchanged == books, everything else zero.
  uint16_t unchanged = 0;  // same (name, size): keeps its place in "Recently added"
  uint16_t added = 0;      // matched nothing, not even by size
  uint16_t renamed = 0;    // matched a leftover entry by size alone
  uint16_t removed = 0;    // previous entry no book claimed
  uint16_t enriched = 0;   // took its title or author from the book rather than the filename
  bool ranksDegraded = false;
  bool dedupDegraded = false;
  bool booksAtRoot = false;
};

// Progress callback, invoked as folders are entered so a long first scan can
// show something. Returning false aborts the build and leaves any previous
// index in place. A function pointer, not std::function: this runs during a
// blocking phase where the repo's rules forbid heap churn.
using BuildProgressFn = bool (*)(uint16_t booksSoFar, const char* currentPath, void* ctx);

// Walk `rootPath`, write `/.crosspoint/library.idx`, and report what happened.
// `previousNextFirstSeen` carries the monotonic counter across rebuilds so
// "recently added" ordering survives; pass 0 on a first build.
// `readMetadata` makes the walk prefer the title and author held inside each
// book over its filename. It reads an existing cache when available; otherwise
// it stops the normal EPUB parser at the end of <metadata>, before the manifest,
// without building the reader's spine, TOC, CSS, or section caches.
bool buildLibraryIndex(const char* rootPath, uint16_t previousNextFirstSeen, BuildStats& stats,
                       bool readMetadata = false, BuildProgressFn onProgress = nullptr, void* progressCtx = nullptr);

// Paths, exposed so the activity and the tests agree on them.
const char* libraryIndexPath();
const char* libraryStagePath();

}  // namespace library
