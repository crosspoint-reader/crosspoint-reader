#pragma once
// Builds the CLX1 index by walking the SD card once.
//
// The walk derives filename fallbacks and can read title, author and grouping
// metadata from EPUBs. Fresh metadata is reused from the previous index.
//
// Shape of the build, and why:
//
//   * ONE walk. Records go straight into a staging file in discovery order, so
//     nothing proportional to the library stays resident. Phase-local sort arrays
//     are fallible and released before the next large allocation.
//   * Duplicate directory entries are dropped. A damaged FAT can hand the same
//     file out twice — measured on a real card: 6 of 75 entries were duplicate
//     dirents resolving to one inode — and without this the shelf shows phantom
//     books that cannot be opened.
//   * Unreadable entries are skipped, never fatal. The same card had 7 entries
//     whose names enumerate but whose contents cannot be opened.
//   * Install is write-then-rename, so an interrupted build leaves the previous
//     index untouched rather than a half-written one.

#include <cstdint>

#include "LibraryFormat.h"

namespace library {

// Directory levels below the scan root that are walked. The measured corpus is
// two deep (genre/author/book); the cap exists because a corrupted FAT can
// contain a directory that contains itself — also measured on the same card —
// and an uncapped walk would never return.
inline constexpr int LIBRARY_MAX_DEPTH = 5;

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
  uint16_t unchanged = 0;       // same full path: keeps its place in "Recently added"
  uint16_t added = 0;           // matched nothing, not even by size
  uint16_t renamed = 0;         // matched a leftover entry by size alone
  uint16_t removed = 0;         // previous entry no book claimed
  uint16_t enriched = 0;        // took its title or author from the book rather than the filename
  uint16_t parsed = 0;          // EPUB package reads attempted by this build, including failures
  uint16_t metadataCached = 0;  // metadata loaded without a package read
  uint16_t metadataReused = 0;  // carried over from the previous index
  uint16_t groups = 0;          // distinct groups for the configured field
  uint16_t grouped = 0;         // books assigned to a group
  bool indexReplaced = false;
  bool ranksDegraded = false;
  // Group table allocation failed. The index retains the requested kind and source fields.
  bool groupsDegraded = false;
  bool dedupDegraded = false;
};

// Walk `rootPath`, write `/.crosspoint/library.idx`, and report what happened.
// The previous index, including its monotonic "recently added" counter, is read
// internally so callers cannot accidentally split one rebuild state across two
// file opens.
// `readMetadata` makes the walk prefer the title and author held inside each
// book over its filename. It reads an existing cache when available and not
// stale; otherwise it stops the normal EPUB parser at the end of <metadata>,
// before the manifest, without building the reader's spine, TOC, CSS, or section
// caches.
// `groupKind` selects the grouping field and requires `readMetadata`. The reader's
// cache doesn't have group fields so grouped builds always read the package document.
// When kind is `None`, grouping (and group reading/building) is disabled completely.
// When grouping is enabled, grouped builds store all four fields so they can be
// reused if the kind changes without a complete rebuild (however enabling grouping
// after None requires rebuilding since `None` nukes the build). Group allocation
// failure still writes an index but sets `groupsDegraded`.
bool buildLibraryIndex(const char* rootPath, BuildStats& stats, bool readMetadata, GroupKind groupKind);

// Live index path, shared by the builder and activity.
const char* libraryIndexPath();

// A successful book transfer marks the retained index stale. The next Library
// entry rebuilds it through the normal reconciliation path.
bool markLibraryIndexDirty();
bool isLibraryIndexDirty();

}  // namespace library
