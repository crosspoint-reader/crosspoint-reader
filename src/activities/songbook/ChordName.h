#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace songbook {

enum class ChordQuality : uint8_t {
  Major,
  Minor,
  Dominant7,
  Minor7,
  Major7,
  Sixth,
  Minor6,
  Ninth,
  Add9,
  Sus2,
  Sus4,
  Dominant7Sus4,
  Diminished,
  Diminished7,
  Augmented,
  HalfDiminished,  // Keep last; CHORD_QUALITY_COUNT counts up to it.
};
constexpr size_t CHORD_QUALITY_COUNT = 16;
static_assert(static_cast<size_t>(ChordQuality::HalfDiminished) + 1 == CHORD_QUALITY_COUNT);

constexpr int8_t NO_BASS = -1;

struct ChordName {
  // Pitch class, C = 0.
  uint8_t root = 0;
  ChordQuality quality = ChordQuality::Major;
  // Pitch class after '/', or NO_BASS.
  int8_t bass = NO_BASS;

  bool operator==(const ChordName& other) const {
    return root == other.root && quality == other.quality && bass == other.bass;
  }
};

// Parses names like "F#m7", "Bb/D", or "Hm" (H is B, as in Russian songbooks;
// B is also B). False for text that is not a chord or has an unsupported quality.
bool parseChordName(std::string_view text, ChordName& out);

// Chord tones as a 12-bit mask of semitones above the root (bit 0 = root).
uint16_t chordIntervals(ChordQuality quality);

}  // namespace songbook
