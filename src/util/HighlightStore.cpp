#include "HighlightStore.h"

#include <HalStorage.h>
#include <Logging.h>

#include "CrossPointSettings.h"
#include "StringUtils.h"

namespace {

constexpr const char* HIGHLIGHTS_DIR = "/Highlights";
constexpr const char* SINGLE_FILE_PATH = "/Highlights.md";

// Last successful append, used to group consecutive passages under one
// heading without re-reading the file. Static (not per-activity) so the
// grouping survives re-entering the reader; a reboot just re-writes one
// heading, which markdown tolerates. Bounded by title lengths (~100 bytes).
std::string memoPath;
std::string memoBook;
std::string memoChapter;

}  // namespace

namespace HighlightStore {

bool save(const std::string& bookTitle, const std::string& chapterTitle, const std::string& passage) {
  if (passage.empty()) return false;
  const std::string book = bookTitle.empty() ? "Untitled" : bookTitle;

  const bool perBook = SETTINGS.highlightFileMode == CrossPointSettings::HIGHLIGHT_FILE_PER_BOOK;
  std::string path;
  if (perBook) {
    if (!Storage.ensureDirectoryExists(HIGHLIGHTS_DIR)) {
      LOG_ERR("HILITE", "Cannot create %s", HIGHLIGHTS_DIR);
      return false;
    }
    path = HIGHLIGHTS_DIR;
    path += '/';
    path += StringUtils::sanitizeFilename(book);
    path += ".md";
  } else {
    path = SINGLE_FILE_PATH;
  }

  HalFile file = Storage.open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND);
  if (!file) {
    LOG_ERR("HILITE", "Cannot open %s", path.c_str());
    return false;
  }
  const bool fresh = file.fileSize() == 0;
  const bool sameBook = !fresh && path == memoPath && book == memoBook;
  const bool sameChapter = sameBook && chapterTitle == memoChapter;

  // Single write: "# book" when the file is new or (single-file mode) the
  // book changed, "## chapter" when the chapter changed, then the passage.
  std::string out;
  out.reserve(book.size() + chapterTitle.size() + passage.size() + 16);
  if (fresh || (!perBook && !sameBook)) {
    if (!fresh) out += '\n';
    out += "# ";
    out += book;
    out += '\n';
  }
  if (!chapterTitle.empty() && !sameChapter) {
    out += "\n## ";
    out += chapterTitle;
    out += '\n';
  }
  out += "\n> ";
  out += passage;
  out += '\n';

  if (file.write(out.data(), out.size()) != out.size()) {
    LOG_ERR("HILITE", "Write failed: %s", path.c_str());
    return false;
  }
  memoPath = std::move(path);
  memoBook = book;
  memoChapter = chapterTitle;
  return true;
}

}  // namespace HighlightStore
