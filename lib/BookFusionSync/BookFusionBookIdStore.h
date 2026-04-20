#pragma once
#include <cstddef>
#include <cstdint>

/**
 * Per-book sidecar for BookFusion book IDs.
 *
 * Each EPUB that was downloaded from BookFusion has an associated sidecar file
 * at /.crosspoint/bookfusion_<md5_of_epub_path>.json containing its numeric book_id.
 *
 * Returns 0 from loadBookId() when no sidecar exists — 0 is never a valid BookFusion ID.
 */
class BookFusionBookIdStore {
 public:
  // Load book_id for the given epub path. Returns 0 if not a BookFusion book.
  static uint32_t loadBookId(const char* epubPath);

  // Save book_id for an epub path. Returns false on I/O error or if id == 0.
  static bool saveBookId(const char* epubPath, uint32_t bookId);

 private:
  // Derives /.crosspoint/bookfusion_<32hexchars>.json from the epub path.
  static void buildSidecarPath(const char* epubPath, char* outPath, size_t maxLen);
};
