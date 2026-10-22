#pragma once

#include <string>
#include <vector>

#include "Setlist.h"
#include "Song.h"

namespace songbook {

constexpr char ROOT_DIR[] = "/songbook";
constexpr char SONGS_DIR[] = "/songbook/songs";
constexpr char SETLISTS_DIR[] = "/songbook/setlists";
// Real songs are a few KB; the cap bounds the parsed pool for a wrong file.
constexpr size_t MAX_SONG_FILE_BYTES = 64 * 1024;
constexpr size_t MAX_SETLIST_FILE_BYTES = 16 * 1024;
constexpr int MAX_LISTED_FILES = 200;

enum class SongLoadResult { Ok, Missing, TooLarge, Empty, OutOfMemory };

enum class SetlistStatus { Ok, Unreadable, NoSongs };

struct SetlistFile {
  std::string file;  // name inside SETLISTS_DIR
  std::string title;
  SetlistStatus status = SetlistStatus::Unreadable;
};

bool songbookFolderExists();
// relativePath is under SONGS_DIR. The title falls back to the file name.
SongLoadResult loadSong(const std::string& relativePath, Song& song, bool metadataOnly);
// Song files directly in SONGS_DIR (the first MAX_LISTED_FILES entries), sorted by name.
void listSongFiles(std::vector<std::string>& out);
bool hasSongFiles();
// Setlist files in SETLISTS_DIR with their titles, sorted by file name.
void listSetlists(std::vector<SetlistFile>& out);
// False when the file cannot be read; result tells a parse failure apart.
bool loadSetlist(const std::string& file, Setlist& out, SetlistParseResult& result);

}  // namespace songbook
