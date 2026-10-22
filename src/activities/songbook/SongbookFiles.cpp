#include "SongbookFiles.h"

#include <HalMemory.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <strings.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <iterator>

#include "ChordProParser.h"

namespace songbook {

namespace {

constexpr size_t READ_CHUNK_BYTES = 512;
constexpr size_t NAME_BUFFER_BYTES = 256;
// Kept free after the parsed song; the layouts check their own budget.
constexpr size_t LOAD_HEADROOM_BYTES = 8 * 1024;

std::string fileStem(const std::string& path) {
  const size_t slash = path.rfind('/');
  std::string stem = path.substr(slash == std::string::npos ? 0 : slash + 1);
  const size_t dot = stem.rfind('.');
  if (dot != std::string::npos && dot > 0) stem.resize(dot);
  return stem;
}

bool lessIgnoreCase(const std::string& a, const std::string& b) { return strcasecmp(a.c_str(), b.c_str()) < 0; }

bool endsWithJson(const char* name) {
  const size_t length = strlen(name);
  return length > 5 && strcasecmp(name + length - 5, ".json") == 0;
}

// Calls onName(name) for each visible regular file in dir until it returns
// false; false if dir is unreadable.
template <typename OnName>
bool forEachFile(const char* dir, OnName onName) {
  auto root = Storage.open(dir);
  if (!root || !root.isDirectory()) return false;
  auto name = makeUniqueNoThrow<char[]>(NAME_BUFFER_BYTES);
  if (!name) {
    LOG_ERR("SONG", "OOM: %u byte name buffer", static_cast<unsigned>(NAME_BUFFER_BYTES));
    return false;
  }
  root.rewindDirectory();
  int seen = 0;
  for (auto file = root.openNextFile(); file && seen < MAX_LISTED_FILES; file = root.openNextFile()) {
    if (file.isDirectory()) continue;
    file.getName(name.get(), NAME_BUFFER_BYTES);
    if (name[0] == '.') continue;
    ++seen;
    if (!onName(name.get())) break;
  }
  return true;
}

}  // namespace

bool songbookFolderExists() { return Storage.exists(ROOT_DIR); }

SongLoadResult loadSong(const std::string& relativePath, Song& song, const bool metadataOnly) {
  song.clear();
  if (!isSafeSongPath(relativePath)) return SongLoadResult::Missing;
  const std::string path = std::string(SONGS_DIR) + "/" + relativePath;
  HalFile file;
  if (!Storage.openFileForRead("SONG", path, file) || file.isDirectory()) return SongLoadResult::Missing;
  const size_t size = file.fileSize();
  if (size > MAX_SONG_FILE_BYTES) {
    LOG_ERR("SONG", "Song too large (%u bytes): %s", static_cast<unsigned>(size), path.c_str());
    return SongLoadResult::TooLarge;
  }
  auto chunk = makeUniqueNoThrow<char[]>(READ_CHUNK_BYTES);
  if (!chunk) {
    LOG_ERR("SONG", "OOM: %u byte read buffer", static_cast<unsigned>(READ_CHUNK_BYTES));
    return SongLoadResult::OutOfMemory;
  }

  if (!metadataOnly) {
    // Upper bounds from a counting pass size every container once.
    size_t newlines = 1;
    size_t brackets = 0;
    size_t braces = 0;
    for (int n = file.read(chunk.get(), READ_CHUNK_BYTES); n > 0; n = file.read(chunk.get(), READ_CHUNK_BYTES)) {
      for (int i = 0; i < n; ++i) {
        newlines += chunk[i] == '\n';
        brackets += chunk[i] == '[';
        braces += chunk[i] == '{';
      }
    }
    file.seekSet(0);
    // Container growth aborts on OOM with exceptions off, so the whole parse
    // is reserved up front and checked against the heap first.
    const size_t textBytes = std::min(size, ChordProParser::MAX_TEXT_BYTES);
    const size_t lineCount = std::min(newlines, ChordProParser::MAX_LINES);
    const size_t chordCount = std::min(brackets, ChordProParser::MAX_CHORDS);
    const size_t sectionCount = std::min(braces + 1, ChordProParser::MAX_SECTIONS);
    const size_t chartCount = std::min(newlines, ChordProParser::MAX_CHART_ROWS);
    const size_t lineBytes = lineCount * sizeof(SongLine);
    const size_t chordBytes = chordCount * sizeof(ChordAnchor);
    const size_t sectionBytes = sectionCount * sizeof(Section);
    const size_t chartBytes = chartCount * sizeof(ChartRow);
    const auto heap = HalMemory::getDefaultHeap();
    // The largest block must hold the two biggest reserves together (fragmentation).
    size_t sizes[] = {textBytes, lineBytes, chordBytes, sectionBytes, chartBytes};
    std::sort(std::begin(sizes), std::end(sizes), std::greater<>());
    if (heap.freeBytes < textBytes + lineBytes + chordBytes + sectionBytes + chartBytes + LOAD_HEADROOM_BYTES ||
        heap.largestBlockBytes < sizes[0] + sizes[1]) {
      LOG_ERR("SONG", "OOM: free %u, largest %u for %u byte song", static_cast<unsigned>(heap.freeBytes),
              static_cast<unsigned>(heap.largestBlockBytes), static_cast<unsigned>(size));
      return SongLoadResult::OutOfMemory;
    }
    song.text.reserve(textBytes);
    song.lines.reserve(lineCount);
    song.chords.reserve(chordCount);
    song.sections.reserve(sectionCount);
    song.chart.reserve(chartCount);
  }

  ChordProParser parser(song, metadataOnly);
  std::string line;
  line.reserve(ChordProParser::MAX_LINE_BYTES + 1);
  bool reading = true;
  while (reading) {
    const int n = file.read(chunk.get(), READ_CHUNK_BYTES);
    if (n <= 0) break;
    for (int i = 0; i < n && reading; ++i) {
      if (chunk[i] == '\n') {
        reading = parser.feedLine(line);
        line.clear();
      } else if (line.size() <= ChordProParser::MAX_LINE_BYTES) {
        line.push_back(chunk[i]);
      }
    }
  }
  if (reading && !line.empty()) parser.feedLine(line);
  parser.finish();

  if (parser.truncated()) LOG_ERR("SONG", "Song content truncated at parser limits: %s", path.c_str());
  if (song.meta.title.empty()) song.meta.title = fileStem(relativePath);
  if (!metadataOnly && song.lines.empty() && song.chart.empty()) return SongLoadResult::Empty;
  return SongLoadResult::Ok;
}

void listSongFiles(std::vector<std::string>& out) {
  out.clear();
  forEachFile(SONGS_DIR, [&out](const char* name) {
    if (hasSongExtension(name)) out.emplace_back(name);
    return true;
  });
  std::sort(out.begin(), out.end(), lessIgnoreCase);
}

bool hasSongFiles() {
  bool found = false;
  forEachFile(SONGS_DIR, [&found](const char* name) {
    found = hasSongExtension(name);
    return !found;
  });
  return found;
}

void listSetlists(std::vector<SetlistFile>& out) {
  out.clear();
  forEachFile(SETLISTS_DIR, [&out](const char* name) {
    if (endsWithJson(name)) out.push_back(SetlistFile{name, {}, SetlistStatus::Unreadable});
    return true;
  });
  std::sort(out.begin(), out.end(),
            [](const SetlistFile& a, const SetlistFile& b) { return lessIgnoreCase(a.file, b.file); });
  Setlist setlist;
  for (auto& entry : out) {
    SetlistParseResult result = SetlistParseResult::InvalidJson;
    if (loadSetlist(entry.file, setlist, result)) {
      switch (result) {
        case SetlistParseResult::Ok:
          entry.status = SetlistStatus::Ok;
          break;
        case SetlistParseResult::NoSongs:
          entry.status = SetlistStatus::NoSongs;
          break;
        case SetlistParseResult::InvalidJson:
          entry.status = SetlistStatus::Unreadable;
          break;
      }
    }
    entry.title = setlist.title.empty() ? fileStem(entry.file) : setlist.title;
  }
}

bool loadSetlist(const std::string& file, Setlist& out, SetlistParseResult& result) {
  out.title.clear();
  out.songs.clear();
  std::string json;
  if (!Storage.readFileToString("SONG", std::string(SETLISTS_DIR) + "/" + file, MAX_SETLIST_FILE_BYTES, json)) {
    LOG_ERR("SONG", "Cannot read setlist %s", file.c_str());
    return false;
  }
  result = parseSetlist(json, out);
  if (result != SetlistParseResult::Ok) LOG_ERR("SONG", "Invalid setlist %s", file.c_str());
  return true;
}

}  // namespace songbook
