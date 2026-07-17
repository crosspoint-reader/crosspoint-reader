#pragma once
#include <cstdint>
#include <string>

// On-disk filename format for books downloaded from an OPDS server. Stored as a
// uint8_t in CrossPointSettings; cast to this enum at the call sites. `Count` is
// the number of selectable formats (used to cycle the setting in the UI).
enum class OpdsFilenameFormat : uint8_t {
  AuthorTitle = 0,  // "Author - Title.epub" (default; matches legacy behaviour)
  TitleAuthor = 1,  // "Title - Author.epub"
  TitleOnly = 2,    // "Title.epub"
  Count = 3,
};

// Composes and sanitizes the on-disk filename (including the ".epub" extension)
// for a downloaded OPDS book, according to `format`. When the author is empty,
// every format collapses to just the sanitized title. Pure: no I/O, no globals.
std::string opdsBookFilename(const std::string& author, const std::string& title, OpdsFilenameFormat format);
