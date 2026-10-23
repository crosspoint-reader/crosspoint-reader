#include "ChordName.h"

#include <iterator>

namespace songbook {

namespace {

struct QualitySpelling {
  std::string_view text;
  ChordQuality quality;
};

// Suffixes are case-sensitive: "m" is minor, "M" is major.
constexpr QualitySpelling SPELLINGS[] = {
    {"", ChordQuality::Major},
    {"M", ChordQuality::Major},
    {"maj", ChordQuality::Major},
    {"m", ChordQuality::Minor},
    {"min", ChordQuality::Minor},
    {"-", ChordQuality::Minor},
    {"7", ChordQuality::Dominant7},
    {"m7", ChordQuality::Minor7},
    {"min7", ChordQuality::Minor7},
    {"-7", ChordQuality::Minor7},
    {"maj7", ChordQuality::Major7},
    {"Maj7", ChordQuality::Major7},
    {"M7", ChordQuality::Major7},
    {"ma7", ChordQuality::Major7},
    {"\xCE\x94", ChordQuality::Major7},   // Δ
    {"\316\2247", ChordQuality::Major7},  // Δ7
    {"6", ChordQuality::Sixth},
    {"m6", ChordQuality::Minor6},
    {"min6", ChordQuality::Minor6},
    {"9", ChordQuality::Ninth},
    {"add9", ChordQuality::Add9},
    {"add2", ChordQuality::Add9},
    {"sus2", ChordQuality::Sus2},
    {"sus4", ChordQuality::Sus4},
    {"sus", ChordQuality::Sus4},
    {"7sus4", ChordQuality::Dominant7Sus4},
    {"7sus", ChordQuality::Dominant7Sus4},
    {"dim", ChordQuality::Diminished},
    {"o", ChordQuality::Diminished},
    {"\xC2\xB0", ChordQuality::Diminished},  // °
    {"dim7", ChordQuality::Diminished7},
    {"o7", ChordQuality::Diminished7},
    {"\302\2607", ChordQuality::Diminished7},  // °7
    {"aug", ChordQuality::Augmented},
    {"+", ChordQuality::Augmented},
    {"m7b5", ChordQuality::HalfDiminished},
    {"m7-5", ChordQuality::HalfDiminished},
    {"min7b5", ChordQuality::HalfDiminished},
    {"\xC3\xB8", ChordQuality::HalfDiminished},   // ø
    {"\303\2707", ChordQuality::HalfDiminished},  // ø7
};

// Indexed by ChordQuality.
constexpr uint16_t INTERVALS[] = {
    1u << 0 | 1u << 4 | 1u << 7,                       // Major
    1u << 0 | 1u << 3 | 1u << 7,                       // Minor
    1u << 0 | 1u << 4 | 1u << 7 | 1u << 10,            // Dominant7
    1u << 0 | 1u << 3 | 1u << 7 | 1u << 10,            // Minor7
    1u << 0 | 1u << 4 | 1u << 7 | 1u << 11,            // Major7
    1u << 0 | 1u << 4 | 1u << 7 | 1u << 9,             // Sixth
    1u << 0 | 1u << 3 | 1u << 7 | 1u << 9,             // Minor6
    1u << 0 | 1u << 2 | 1u << 4 | 1u << 7 | 1u << 10,  // Ninth
    1u << 0 | 1u << 2 | 1u << 4 | 1u << 7,             // Add9
    1u << 0 | 1u << 2 | 1u << 7,                       // Sus2
    1u << 0 | 1u << 5 | 1u << 7,                       // Sus4
    1u << 0 | 1u << 5 | 1u << 7 | 1u << 10,            // Dominant7Sus4
    1u << 0 | 1u << 3 | 1u << 6,                       // Diminished
    1u << 0 | 1u << 3 | 1u << 6 | 1u << 9,             // Diminished7
    1u << 0 | 1u << 4 | 1u << 8,                       // Augmented
    1u << 0 | 1u << 3 | 1u << 6 | 1u << 10,            // HalfDiminished
};

static_assert(std::size(INTERVALS) == CHORD_QUALITY_COUNT);

bool isSpace(const char c) { return c == ' ' || c == '\t'; }

// Reads a note letter with an optional accidental from the front of text.
bool takeNote(std::string_view& text, uint8_t& pitch) {
  // Pitch classes of A..H; H is the German and Russian name of B.
  static constexpr uint8_t LETTERS[] = {9, 11, 0, 2, 4, 5, 7, 11};
  if (text.empty() || text.front() < 'A' || text.front() > 'H') return false;
  int value = LETTERS[text.front() - 'A'];
  text.remove_prefix(1);
  if (!text.empty() && text.front() == '#') {
    ++value;
    text.remove_prefix(1);
  } else if (!text.empty() && text.front() == 'b') {
    --value;
    text.remove_prefix(1);
  } else if (text.substr(0, 3) == "\xE2\x99\xAF") {  // ♯
    ++value;
    text.remove_prefix(3);
  } else if (text.substr(0, 3) == "\xE2\x99\xAD") {  // ♭
    --value;
    text.remove_prefix(3);
  }
  pitch = static_cast<uint8_t>((value + 12) % 12);
  return true;
}

}  // namespace

bool parseChordName(std::string_view text, ChordName& out) {
  while (!text.empty() && isSpace(text.front())) text.remove_prefix(1);
  while (!text.empty() && isSpace(text.back())) text.remove_suffix(1);

  ChordName chord;
  if (!takeNote(text, chord.root)) return false;

  std::string_view suffix = text;
  const size_t slash = text.find('/');
  if (slash != std::string_view::npos) {
    suffix = text.substr(0, slash);
    std::string_view bassText = text.substr(slash + 1);
    uint8_t bass = 0;
    if (!takeNote(bassText, bass) || !bassText.empty()) return false;
    chord.bass = static_cast<int8_t>(bass);
  }

  for (const auto& spelling : SPELLINGS) {
    if (spelling.text != suffix) continue;
    chord.quality = spelling.quality;
    out = chord;
    return true;
  }
  return false;
}

uint16_t chordIntervals(const ChordQuality quality) { return INTERVALS[static_cast<size_t>(quality)]; }

}  // namespace songbook
