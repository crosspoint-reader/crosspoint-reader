#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "ClippingCodec.h"

class ClippingStore {
 public:
  static constexpr size_t MAX_CATALOG_BOOKS = 32;
  static constexpr size_t MAX_CATALOG_DIRECTORY_ENTRIES = 128;

  enum class LoadResult : uint8_t {
    Ready,
    Loaded,
    Recovered,
    Migrated,
    LoadedLegacy,
    UnsupportedVersion,
    InvalidFile,
    IoError,
  };

  enum class AddResult : uint8_t {
    Added,
    LimitReached,
    InvalidData,
    SaveFailed,
  };

  enum class RekeyResult : uint8_t {
    Rekeyed,
    Prepared,
    Unchanged,
    NotLoaded,
    NotPrepared,
    InvalidDestination,
    SourceInvalid,
    DestinationExists,
    UnsupportedVersion,
    IoError,
  };

  enum class CatalogLoadResult : uint8_t {
    Loaded,
    DirectoryMissing,
    IoError,
  };

  enum class ExportResult : uint8_t {
    Exported,
    Empty,
    CatalogIncomplete,
    SourceChanged,
    NoAvailableName,
    IoError,
  };

  struct CatalogEntry {
    ClippingCodec::BookMetadata book;
    std::string sourcePath;
    ClippingCodec::Format format = ClippingCodec::Format::Current;
    uint32_t fileLength = 0;
    uint32_t newestTimestamp = 0;
    uint16_t clippingCount = 0;
    bool bookExists = false;
  };

  struct Catalog {
    std::vector<CatalogEntry> entries;
    uint16_t skippedBooks = 0;
    bool directoryTruncated = false;
    bool entryNameTruncated = false;
  };

  LoadResult loadForBook(const std::string& filePath, const std::string& title, const std::string& author,
                         const std::string& bookType = "epub");
  // Removes canonical/backup/temp only after every existing file validates as
  // belonging to this exact path and type. A .move marker is removed only when
  // its identity names this exact destination. Corrupt, unreadable, newer,
  // mismatched, or CRC-colliding data is preserved rather than guessed at.
  static bool removeFilesForBook(const std::string& filePath, const std::string& bookType = "epub");
  static bool hasFilesForBook(const std::string& filePath, const std::string& bookType = "epub");
  // Moves every exactly-owned canonical/backup/temp/move file into an inactive
  // directory without deleting it. Each rename is independently verifiable and
  // the operation may be retried after power loss. Unreadable, corrupt, newer,
  // or mismatched files fail closed and remain in their current location.
  static bool quarantineFilesForBook(const std::string& filePath, const std::string& quarantineDirectory,
                                     const std::string& bookType = "epub");
  // Scans exact canonical/.bak/.tmp clipping names without changing, promoting,
  // or migrating any file. At most MAX_CATALOG_BOOKS valid book stores are
  // retained; the report exposes every bounded/truncated case to callers.
  static CatalogLoadResult loadCatalog(Catalog& catalog);
  // Revalidates and streams a complete catalog into a new root-level text file.
  // Existing exports and CrossInk's /My Clippings.txt are never overwritten.
  static ExportResult exportCatalog(const Catalog& catalog, std::string& outputPath);
  void unload();

  AddResult add(const ClippingCodec::ClippingMetadata& clipping, std::string_view text);
  bool remove(size_t index);
  bool readText(size_t index, std::string& out) const;

  // Phase 1 of a crash-safe book move. Builds and verifies the destination
  // while leaving the source canonical authoritative and readable. Call
  // finalizePreparedRekey() only after the EPUB rename has succeeded, or
  // cancelPreparedRekey() when it failed.
  RekeyResult prepareRekeyForBook(const std::string& filePath, const std::string& title, const std::string& author,
                                  const std::string& bookType = "epub");
  RekeyResult finalizePreparedRekey();
  bool cancelPreparedRekey();
  bool hasPreparedRekey() const { return !preparedDestinationPath_.empty(); }

  // One-shot compatibility wrapper. Book moves should use the two-phase API
  // above so the destination is durable before the EPUB is renamed.
  RekeyResult rekeyForBook(const std::string& filePath, const std::string& title, const std::string& author,
                           const std::string& bookType = "epub");

  bool isLoaded() const { return loaded_; }
  size_t size() const { return index_.clippings.size(); }
  const ClippingCodec::ClippingMetadata* at(size_t index) const;
  const std::vector<ClippingCodec::ClippingMetadata>& entries() const { return index_.clippings; }
  const ClippingCodec::BookMetadata& book() const { return book_; }
  ClippingCodec::Format format() const { return index_.format; }
  uint32_t fileLength() const { return index_.fileLength; }
  ClippingCodec::Status lastCodecStatus() const { return lastCodecStatus_; }
  const std::string& path() const { return storePath_; }

 private:
  ClippingCodec::BookMetadata book_;
  ClippingCodec::Index index_;
  std::string storePath_;
  std::string preparedDestinationPath_;
  ClippingCodec::Index preparedIndex_;
  bool loaded_ = false;
  ClippingCodec::Status lastCodecStatus_ = ClippingCodec::Status::Ok;

  bool rewrite(std::vector<ClippingCodec::ClippingMetadata> target, size_t replacementIndex,
               const std::string_view* replacementText);
};
