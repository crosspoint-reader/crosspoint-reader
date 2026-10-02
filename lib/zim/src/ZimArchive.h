// Pocket Library for CrossPoint
// Copyright (C) 2026 Pocket Library contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. See LICENSE-GPL-3.0 at the repository root.

#pragma once

// Reader for openZIM archives (the format Kiwix publishes Wikipedia in).
// Written from the openZIM file-format description and checked against the
// openZIM test files; no code from libzim or other readers.
//
// Design limits for a 380 KB-SRAM / 8 MB-PSRAM device:
//  * no exceptions; every operation returns an Error;
//  * nothing proportional to the number of entries is held in memory
//    (English Wikipedia has ~18 M entries); lookups binary-search on disk;
//  * decompressed clusters (up to a few MB) come from the Allocator and are
//    kept in a small LRU cache; uncompressed clusters are never loaded whole.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ZimSource.h"

namespace zim {

enum class Error : uint8_t {
  None = 0,
  Io,              // Source read failed or was short
  BadMagic,        // not a ZIM file
  BadVersion,      // unsupported major version
  BadHeader,       // header fields point outside the file or are inconsistent
  BadDirent,       // malformed directory entry
  BadCluster,      // malformed cluster or blob table
  Unsupported,     // compression type we don't decode
  Decompress,      // compressed data is corrupt
  TooLarge,        // exceeds Options limits
  NoMemory,        // allocator returned null
  NotFound,        // no such entry
  OutOfRange,      // index beyond the entry/title/blob count
  RedirectLoop,    // redirect chain longer than Options::maxRedirects
  NotContent,      // entry is a redirect/link/deleted entry where content was expected
};

const char* errorName(Error e);

struct Header {
  uint16_t majorVersion = 0;
  uint16_t minorVersion = 0;
  uint8_t uuid[16] = {};
  uint32_t entryCount = 0;
  uint32_t clusterCount = 0;
  uint64_t pathPtrPos = 0;
  uint64_t titlePtrPos = 0;
  uint64_t clusterPtrPos = 0;
  uint64_t mimeListPos = 0;
  uint32_t mainPage = 0xffffffffu;
  uint32_t layoutPage = 0xffffffffu;
  uint64_t checksumPos = 0;
};

// Special MIME indices used in directory entries.
constexpr uint16_t kMimeRedirect = 0xffff;
constexpr uint16_t kMimeLinkTarget = 0xfffe;  // old namespace scheme only
constexpr uint16_t kMimeDeleted = 0xfffd;     // old namespace scheme only
constexpr uint32_t kNoEntry = 0xffffffffu;

struct Entry {
  uint32_t index = kNoEntry;  // position in the path-ordered directory
  uint16_t mime = 0;
  char ns = 0;
  uint32_t revision = 0;
  uint32_t redirectIndex = kNoEntry;  // valid when isRedirect()
  uint32_t cluster = 0;               // valid when isContent()
  uint32_t blob = 0;
  std::string path;
  std::string title;  // the entry's title, or its path when the title is empty

  bool isRedirect() const { return mime == kMimeRedirect; }
  bool isContent() const { return mime < kMimeDeleted; }
};

struct Options {
  // Clusters that would decompress beyond this are refused (protects the
  // device from corrupt or hostile size fields). Kiwix uses ~2 MiB.
  size_t maxClusterBytes = 16u * 1024u * 1024u;
  // Compressed clusters beyond this are refused before reading them.
  size_t maxCompressedClusterBytes = 16u * 1024u * 1024u;
  // Number of decompressed clusters kept (the brief's LRU of 3).
  size_t clusterCacheSize = 3;
  int maxRedirects = 32;
  const Allocator* allocator = nullptr;  // null = malloc/free
};

// Where the title index comes from. Recent Kiwix files (libzim 9+) ship only
// the front-article list, so that is used when nothing fuller exists.
enum class TitleSource : uint8_t {
  None,
  Listing,   // X/listing/titleOrdered/v0: every entry
  Header,    // header title pointer list: every entry
  Articles,  // X/listing/titleOrdered/v1: front articles (and their redirects) only
};

struct CacheStats {
  uint32_t hits = 0;
  uint32_t misses = 0;
  uint32_t decompressions = 0;
};

class Archive {
 public:
  Archive();
  ~Archive();
  Archive(const Archive&) = delete;
  Archive& operator=(const Archive&) = delete;

  Error open(std::unique_ptr<Source> source, const Options& options = Options());
  bool isOpen() const { return source_ != nullptr; }
  void close();

  const Header& header() const { return header_; }
  // Version 6.1+ puts content in 'C' and metadata in 'M'; older files use
  // 'A' for articles, 'I' for images, '-' for layout.
  bool usesNewNamespaces() const { return newNamespaces_; }
  char contentNamespace() const { return newNamespaces_ ? 'C' : 'A'; }
  uint32_t entryCount() const { return header_.entryCount; }
  size_t mimeTypeCount() const { return mimeTypes_.size(); }
  // Empty string for out-of-range indices (including the special values).
  const std::string& mimeType(uint16_t index) const;

  // --- directory -----------------------------------------------------------
  Error entryAt(uint32_t index, Entry& out);
  // Exact lookup by namespace + path (paths are byte-compared, as stored).
  Error findByPath(char ns, std::string_view path, Entry& out);
  // Follows redirects until a non-redirect entry; leaves `entry` on it.
  Error resolve(Entry& entry);
  // The archive's main page, resolved (header mainPage, else W/mainPage).
  Error mainEntry(Entry& out);
  // Metadata such as "Title", "Language", "Date", "Name" (M namespace).
  Error metadata(std::string_view name, std::string& out);

  // --- title index ---------------------------------------------------------
  // Entries ordered by (namespace, title). Source is X/listing/titleOrdered/v0
  // when present, else the header's title pointer list, else the front-article
  // list (v1), which covers only articles but is all recent files carry.
  uint32_t titleCount() const { return titleCount_; }
  TitleSource titleSource() const { return titleSource_; }
  bool hasTitleIndex() const { return titleCount_ > 0; }
  Error titleEntryAt(uint32_t position, Entry& out);
  // First position whose (ns, title) is >= (ns, title). Equals titleCount()
  // when every title is smaller. Byte-wise comparison, so case-sensitive.
  Error lowerBoundTitle(char ns, std::string_view title, uint32_t& position);
  // Exact title match within a namespace.
  Error findByTitle(char ns, std::string_view title, Entry& out);
  // Front articles only (X/listing/titleOrdered/v1), when the file has it.
  bool hasArticleList() const { return articleListCount_ > 0; }
  uint32_t articleListCount() const { return articleListCount_; }
  Error articleListEntryAt(uint32_t position, Entry& out);
  // Entry indices at positions [first, first + count) of the front-article
  // list, in one read (for tools that walk all of it).
  Error articleListIndices(uint32_t first, uint32_t count, uint32_t* out);

  // --- content -------------------------------------------------------------
  // Copies the entry's bytes into `out`. The entry must be content (resolve
  // redirects first).
  Error read(const Entry& entry, std::string& out);
  Error readBlob(uint32_t cluster, uint32_t blob, std::string& out);
  // Part of a blob; used for large listings without loading them whole.
  Error readBlobRange(uint32_t cluster, uint32_t blob, uint64_t offset, size_t len, void* dst);
  Error blobSize(uint32_t cluster, uint32_t blob, uint64_t& size);

  const CacheStats& cacheStats() const { return stats_; }
  void clearCache();

 private:
  struct ClusterInfo {
    uint64_t start = 0;  // offset of the info byte
    uint64_t end = 0;    // exclusive
    uint8_t compression = 0;
    bool extended = false;  // 64-bit blob offsets
  };
  struct CachedCluster {
    uint32_t index = 0;
    uint8_t* data = nullptr;
    size_t size = 0;
    uint64_t lastUse = 0;
  };
  static constexpr size_t kValidatedRing = 16;
  // Offset tables longer than this are checked entry by entry instead of
  // whole (keeps a corrupt count from triggering a huge read).
  static constexpr uint64_t kMaxValidatedTableBytes = 256 * 1024;
  // Resolved location of a blob's bytes, either in a cached buffer or on disk.
  struct BlobLocation {
    const uint8_t* memory = nullptr;  // non-null for decompressed clusters
    uint64_t fileOffset = 0;          // for uncompressed clusters
    uint64_t size = 0;
  };
  struct TitleList {
    // Either an array of uint32 entry indices inside a blob, or the header's
    // title pointer list at an absolute file offset.
    bool inBlob = false;
    uint32_t cluster = 0;
    uint32_t blob = 0;
    uint64_t fileOffset = 0;
  };

  Error readHeader();
  Error readMimeList();
  Error setUpTitleIndex();
  Error findListing(std::string_view path, TitleList& list, uint32_t& count);
  Error readU32At(const TitleList& list, uint32_t position, uint32_t& value);
  Error clusterInfo(uint32_t cluster, ClusterInfo& info);
  Error locateBlob(uint32_t cluster, uint32_t blob, BlobLocation& loc);
  Error validateTable(uint32_t cluster, const uint8_t* memory, uint64_t dataStart, uint64_t dataSize,
                      size_t offSize, uint64_t first);
  Error loadCluster(uint32_t cluster, const ClusterInfo& info, CachedCluster*& out);
  Error decompress(const ClusterInfo& info, uint8_t*& data, size_t& size);
  void releaseCache();
  int comparePathAt(uint32_t index, char ns, std::string_view path, Entry& scratch, Error& err);

  std::unique_ptr<Source> source_;
  Options options_;
  const Allocator* allocator_ = nullptr;
  Header header_;
  bool newNamespaces_ = false;
  std::vector<std::string> mimeTypes_;
  TitleList titles_;
  uint32_t titleCount_ = 0;
  TitleSource titleSource_ = TitleSource::None;
  TitleList articleList_;
  uint32_t articleListCount_ = 0;
  std::vector<CachedCluster> cache_;
  // Clusters whose whole blob-offset table has been checked (small ring).
  uint32_t validated_[kValidatedRing] = {};
  size_t validatedCount_ = 0;
  size_t validatedNext_ = 0;
  uint64_t useCounter_ = 0;
  CacheStats stats_;
};

}  // namespace zim
