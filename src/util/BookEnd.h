#pragma once

// Whether a position is the book's last page: the last page of the last chapter, once that
// chapter's layout is final. Progress is measured at a page's start, so this page alone reads
// short of 100% (78% in a three-page book) although the book has been read to its end.
// A partial layout's page total is still an estimate, hence chapterComplete.
inline bool isLastPageOfBook(const int spine, const int spineCount, const int page, const int totalPages,
                             const bool chapterComplete) {
  return chapterComplete && spineCount > 0 && spine == spineCount - 1 && totalPages > 0 && page >= 0 &&
         page >= totalPages - 1;
}

// The whole-percent figure for a book position measured at the end of its page, as the reader's
// status bar shows it. Rounding must not report a book finished before its end: only a position
// confirmed as the end reads 100%.
inline int bookPercentAtPageEnd(const float bookFraction, const bool atEnd) {
  if (atEnd) return 100;
  const int percent = static_cast<int>(bookFraction * 100.0f + 0.5f);
  return percent < 0 ? 0 : percent > 99 ? 99 : percent;
}
