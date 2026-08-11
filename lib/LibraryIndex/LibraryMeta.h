#pragma once

// Title and author read straight out of an EPUB's package document.
//
// Worth doing because on a real card the metadata is not merely different from
// the filename, it is better: 59 of 60 books carry both fields, the median title
// is 23 characters against 148 for the filename, and it is often more correct —
// one book whose filename says "Germinl" knows itself as "Germinal", another
// whose filename lost its accents knows it is "L'Eneide" with them.
//
// This is the path for books the reader has never opened, which therefore have
// no cache to read. It inflates two small files and stops; it does not index the
// book, which is the seconds-per-book operation the reader performs on open.
//
// Everything here has to survive a damaged card. The library this was written
// against holds 13 EPUBs with corrupt deflate streams and 7 entries that cannot
// be opened at all, so failure is the ordinary case rather than the exotic one:
// every entry point returns false instead of throwing, and a book that cannot be
// read simply keeps its filename.

#include <cstddef>
#include <string>

namespace library {

// How much of a package document is kept in memory.
//
// It used to be 64 KB, which aborted the device: a std::string growing toward
// that size asks for roughly 96 KB contiguous while it still holds the old
// buffer, against a largest-free-block of about 69 KB — and std::string allocates
// by throwing, which on this platform means abort() and a reboot loop.
//
// 8 KB is enough because the fields wanted, dc:title and dc:creator, sit in the
// <metadata> block at the very top of the file. Everything after it is the
// manifest, which is what makes some of these documents 44 KB, and which is read
// and discarded for nothing. The reader now stops at </metadata>.
inline constexpr size_t LIBRARY_OPF_MAX_INFLATED = 8192;
// container.xml is a fixed, tiny file; anything larger is not one.
inline constexpr size_t LIBRARY_CONTAINER_MAX_INFLATED = 8192;

struct BookMetadata {
  std::string title;
  std::string author;
  bool opfTooLarge = false;
};

// Read dc:title and dc:creator from `epubPath`. Returns false when the file is
// not a readable zip, has no locatable package document, or trips a size gate —
// in every case the caller falls back to the filename.
bool readBookMetadata(const std::string& epubPath, BookMetadata& out);

// Full path of the package document, from container.xml. Split out so the
// tolerant-parsing rules are testable on their own.
std::string opfPathFromContainer(const std::string& containerXml);

// First dc:title and dc:creator in package-document XML.
//
// Deliberately not a real XML parser: it scans for the two elements it wants and
// ignores everything else, because a book whose OPF is malformed elsewhere should
// still yield its title rather than nothing. Handles namespace prefixes,
// attributes on the element, and XML entities in the text.
void parseOpfMetadata(const std::string& opfXml, BookMetadata& out);

}  // namespace library
