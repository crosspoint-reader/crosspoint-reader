#pragma once

#include <string>

// Appends reader highlights to markdown on the SD card. Depending on
// SETTINGS.highlightFileMode the passage goes to /Highlights/<book>.md
// (per-book) or /Highlights.md (single file). Passages are grouped under
// "# book" (single-file mode) and "## chapter" headings; a heading is only
// re-written when it differs from the last one already in the file.
namespace HighlightStore {

// The two locations save() writes to, exposed so consumers (the Koofr upload)
// can find the same files without duplicating the paths.
constexpr const char* HIGHLIGHTS_DIR = "/Highlights";
constexpr const char* SINGLE_FILE_PATH = "/Highlights.md";

bool save(const std::string& bookTitle, const std::string& chapterTitle, const std::string& passage);

}  // namespace HighlightStore
