#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace songbook {

// Ordered song files, relative to /songbook/songs/.
struct Setlist {
  std::string title;
  std::vector<std::string> songs;
};

enum class SetlistParseResult { Ok, InvalidJson, NoSongs };

// Hard bound for a hand-edited or corrupt setlist.
constexpr size_t MAX_SETLIST_SONGS = 200;

// {"title": "...", "songs": ["a.cho", "folder/b.chordpro"]}. Unsafe or
// non-string entries are skipped; NoSongs means none were usable.
SetlistParseResult parseSetlist(std::string_view json, Setlist& out);

// Relative path that cannot leave the songs folder: no leading '/', no "..",
// no empty segments, no backslashes or control bytes.
bool isSafeSongPath(std::string_view path);

bool hasSongExtension(std::string_view name);

}  // namespace songbook
