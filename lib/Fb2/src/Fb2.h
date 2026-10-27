#pragma once

// FB2 book orchestration: converts a .fb2 into a synthetic EPUB inside the
// per-book cache directory and hands the stock reader pipeline that EPUB.
// The synthetic filename embeds the source size+mtime, so a changed book
// converts to a new file and the Epub-side cache keys change with it.

#include <string>

class Fb2 {
 public:
  // Cache directory for this book: <cacheRoot>/fb2_<hash(path)>.
  static std::string cacheDirFor(const std::string& fb2Path, const std::string& cacheRoot);

  // Path of the synthetic EPUB for the current source fingerprint, or "" when
  // the book cannot be converted (*error carries the reason).
  static std::string ensureEpub(const std::string& fb2Path, const std::string& cacheRoot,
                                std::string* error = nullptr);
};
