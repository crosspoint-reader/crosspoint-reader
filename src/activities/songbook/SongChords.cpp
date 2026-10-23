#include "SongChords.h"

#include <string_view>

namespace songbook {

namespace {

bool isChartSeparator(const char c) { return c == ' ' || c == '\t' || c == '|' || c == ',' || c == '(' || c == ')'; }

}  // namespace

void SongChords::collect(const Song& song) {
  count = 0;
  cut = false;
  for (const ChordAnchor& anchor : song.chords) {
    if (!anchor.annotation) add(song, anchor.name);
  }
  // x_chart rows hold chords between bar lines and repeat marks like "x2".
  for (const ChartRow& row : song.chart) {
    const std::string_view text = song.view(row.chords);
    size_t start = 0;
    while (start < text.size()) {
      while (start < text.size() && isChartSeparator(text[start])) ++start;
      size_t end = start;
      while (end < text.size() && !isChartSeparator(text[end])) ++end;
      if (end > start) {
        add(song, TextRef{row.chords.offset + static_cast<uint32_t>(start), static_cast<uint16_t>(end - start)});
      }
      start = end;
    }
  }
}

void SongChords::add(const Song& song, const TextRef name) {
  const std::string_view text = song.view(name);
  SongChord entry;
  entry.name = name;
  entry.known = parseChordName(text, entry.chord);
  // An unknown name is kept only if it could be a chord: it starts with a note
  // and has no space. "N.C.", "%", and "Bridge - quiet" are skipped.
  if (!entry.known &&
      (text.empty() || text.front() < 'A' || text.front() > 'H' || text.find(' ') != std::string_view::npos)) {
    return;
  }

  for (size_t i = 0; i < count; ++i) {
    const SongChord& seen = items[i];
    if (seen.known != entry.known) continue;
    if (entry.known ? seen.chord == entry.chord : song.view(seen.name) == text) return;
  }
  if (count == MAX_CHORDS) {
    cut = true;
    return;
  }
  items[count++] = entry;
}

}  // namespace songbook
