#pragma once

#include <cstdint>
#include <string>
#include <string_view>

inline constexpr char BOOK_CACHE_ROOT[] = "/.crosspoint";

// The one list of readable book formats. Switches over it are exhaustive and
// carry no default, so adding a format breaks the build at every site that has
// to handle it.
enum class BookFormat : uint8_t { Unknown, Epub, Xtc, Txt };

BookFormat bookFormat(std::string_view path);

// Every format ReaderActivity can open. BMP and PNG are viewer files, not books.
bool isBook(std::string_view path);

struct BookInfo {
  std::string title;
  std::string author;
  std::string thumbBmpPath;
};

// Never builds a missing cache, so it stays cheap enough for boot: title and
// cover stay blank until the book has been opened once.
BookInfo readBookInfo(const std::string& path);

// TXT covers are sibling image files, offered at full size only.
bool hasThumbnail(BookFormat format);

// Cover thumbnail at a given height. False when the format has none.
bool generateBookThumb(const std::string& path, int height);

// Full-size cover for the sleep screen, empty when there is none.
// `cropped` applies to EPUB only.
std::string generateBookCoverBmp(const std::string& path, bool cropped);
