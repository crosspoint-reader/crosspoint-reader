#pragma once

// Position within the current chapter for reader chrome (menu header, go-to
// percent, progress sync). The section is released while child screens are
// up, so EpubReaderActivity::chapterPosition() falls back to the values cached
// before that reset; this type only carries the numbers and their display
// rules, so the cached position renders exactly like the live one.
struct ChapterPosition {
  int pageIndex = 0;   // 0-based page within the chapter
  int totalPages = 0;  // best-known chapter page count, 0 while unknown

  // 1-based page number as shown to the reader.
  constexpr int displayPage() const { return pageIndex + 1; }
  constexpr bool hasTotal() const { return totalPages > 0; }
  // Fraction of the chapter read, 0 while the total is unknown.
  constexpr float chapterFraction() const {
    return hasTotal() ? static_cast<float>(pageIndex) / static_cast<float>(totalPages) : 0.0f;
  }
};
