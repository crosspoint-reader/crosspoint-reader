#pragma once

#include <string>

// A book renamed or moved on the card keeps what the reader knows about it.
//
// That knowledge is keyed by the book's path: its cache folder (/.crosspoint/epub_<hash of the
// path>, and the xtc_/txt_ equivalents, holding the reading position, bookmarks, laid-out
// chapters and thumbnails), its row in the recent list, its entry in the global bookmark index, and
// the last open book in state.json. Reading stats are keyed by a hash of the file NAME
// (KOReaderDocumentId::calculateFromFilename), so they follow a move by themselves and are carried
// over only when the name changes. Without this a moved book reopened at its first page with no
// bookmarks, and its cache stayed behind under the old path.
//
// Call after the rename itself has succeeded. Files that are not books are ignored.
namespace BookRelocation {

// The cache folder the reader uses for a book at `path`; empty for a file that is not a book.
std::string cacheDirFor(const std::string& path);

void bookMoved(const std::string& oldPath, const std::string& newPath);

// Every book now under `newDir`, which was `oldDir`.
void folderMoved(const std::string& oldDir, const std::string& newDir);

// Either of the two, depending on what is at `newPath` now.
void moved(const std::string& oldPath, const std::string& newPath);

}  // namespace BookRelocation
