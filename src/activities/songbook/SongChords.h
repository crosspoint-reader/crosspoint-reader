#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "ChordName.h"
#include "Song.h"

namespace songbook {

struct SongChord {
  // First spelling seen; "A#" and a later "Bb" share one entry.
  TextRef name;
  ChordName chord;
  // False for a name like "C13" that has no known diagram.
  bool known = false;
};

// Distinct chords of one song: the lyrics' chords in order of first use, then
// any other chords of an x_chart. Fixed storage, so collecting never allocates.
class SongChords {
 public:
  static constexpr size_t MAX_CHORDS = 48;

  void collect(const Song& song);

  size_t size() const { return count; }
  const SongChord& operator[](const size_t index) const { return items[index]; }
  // True when the song had more distinct chords than MAX_CHORDS.
  bool truncated() const { return cut; }

 private:
  void add(const Song& song, TextRef name);

  std::array<SongChord, MAX_CHORDS> items{};
  size_t count = 0;
  bool cut = false;
};

}  // namespace songbook
