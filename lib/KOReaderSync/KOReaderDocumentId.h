#pragma once
#include <string>

/**
 * Calculate KOReader document ID (partial MD5 hash).
 *
 * KOReader identifies documents using a partial MD5 hash of the file content.
 * The algorithm reads 1024 bytes at specific offsets and computes the MD5 hash
 * of the concatenated data.
 *
 * Offsets are calculated as: 1024 << (2*i) for i = -1 to 10
 * Producing: 256, 1024, 4096, 16384, 65536, 262144, 1048576, 4194304,
 *            16777216, 67108864, 268435456, 1073741824 bytes
 *
 * If an offset is beyond the file size, it is skipped.
 */
class KOReaderDocumentId {
 public:
  /**
   * Calculate the KOReader document hash for a file (binary/content-based).
   *
   * @param filePath Path to the file (typically an EPUB)
   * @return 32-character lowercase hex string, or empty string on failure
   */
  static std::string calculate(const std::string& filePath);

  /**
   * Calculate document hash from filename only (filename-based sync mode).
   * This is simpler and works when files have the same name across devices.
   *
   * @param filePath Path to the file (only the filename portion is used)
   * @return 32-character lowercase hex MD5 of the filename
   */
  static std::string calculateFromFilename(const std::string& filePath);

 private:
  // Size of each chunk to read at each offset
  static constexpr size_t CHUNK_SIZE = 1024;

  // Number of offsets to try (i = -1 to 10, so 12 offsets)
  static constexpr int OFFSET_COUNT = 12;

  // Calculate offset for index i: 1024 << (2*i)
  static size_t getOffset(int i);

  // Hash cache helpers
  // Returns the path to the per-book cache file that stores the precomputed hash.
  // Uses the same directory convention as the Epub cache (/.crosspoint/epub_<hash>/).
  static std::string getCacheFilePath(const std::string& filePath);

  // Returns the cached hash if the file size and fingerprint match, or empty
  // string on miss/invalidation.
  //
  // The fingerprint is derived from the file's modification timestamp.  We
  // call `FsFile::getModifyDateTime` to retrieve two 16‑bit packed values
  // supplied by the filesystem: one for the date and one for the time.  These
  // are concatenated and represented as eight hexadecimal digits in the form
  // <date><time> (high 16 bits = packed date, low 16 bits = packed time).
  //
  // The resulting string serves as a lightweight change signal; any modification
  // to the file's mtime will alter the packed date/time combo and invalidate
  // the cache entry.  Since the full document hash is expensive to compute,
  // using the packed timestamp gives us a quick way to detect modifications
  // without reading file contents.
  static std::string loadCachedHash(const std::string& cacheFilePath, size_t fileSize,
                                    const std::string& currentFingerprint);

  // Persists the computed hash alongside the file size and fingerprint (the
  // modification-timestamp token) used to generate it.
  static void saveCachedHash(const std::string& cacheFilePath, size_t fileSize, const std::string& fingerprint,
                             const std::string& hash);
};
