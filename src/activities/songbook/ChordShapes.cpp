#include "ChordShapes.h"

#include <algorithm>
#include <iterator>

namespace songbook {

namespace {

using Q = ChordQuality;

constexpr int8_t X = MUTED;
enum : uint8_t { C = 0, Db = 1, D = 2, Eb = 3, E = 4, F = 5, Gb = 6, G = 7, Ab = 8, A = 9, Bb = 10, B = 11 };

// A movable shape also serves other roots: it is shifted along the neck, and
// its open strings become a barre. Exact roots are looked up first.
constexpr bool M = true;
constexpr bool FIXED = false;

struct Shape {
  uint8_t root;
  ChordQuality quality;
  bool movable;
  int8_t frets[MAX_STRINGS];
};

// Strings E A D G B E.
constexpr Shape GUITAR[] = {
    {C, Q::Major, FIXED, {X, 3, 2, 0, 1, 0}},
    {D, Q::Major, FIXED, {X, X, 0, 2, 3, 2}},
    {E, Q::Major, M, {0, 2, 2, 1, 0, 0}},
    {G, Q::Major, FIXED, {3, 2, 0, 0, 0, 3}},
    {A, Q::Major, M, {X, 0, 2, 2, 2, 0}},

    {A, Q::Minor, M, {X, 0, 2, 2, 1, 0}},
    {D, Q::Minor, FIXED, {X, X, 0, 2, 3, 1}},
    {E, Q::Minor, M, {0, 2, 2, 0, 0, 0}},

    {A, Q::Dominant7, M, {X, 0, 2, 0, 2, 0}},
    {B, Q::Dominant7, FIXED, {X, 2, 1, 2, 0, 2}},
    {C, Q::Dominant7, FIXED, {X, 3, 2, 3, 1, 0}},
    {D, Q::Dominant7, FIXED, {X, X, 0, 2, 1, 2}},
    {E, Q::Dominant7, M, {0, 2, 0, 1, 0, 0}},
    {G, Q::Dominant7, FIXED, {3, 2, 0, 0, 0, 1}},

    {A, Q::Minor7, M, {X, 0, 2, 0, 1, 0}},
    {D, Q::Minor7, FIXED, {X, X, 0, 2, 1, 1}},
    {E, Q::Minor7, M, {0, 2, 0, 0, 0, 0}},

    {A, Q::Major7, M, {X, 0, 2, 1, 2, 0}},
    {C, Q::Major7, FIXED, {X, 3, 2, 0, 0, 0}},
    {D, Q::Major7, FIXED, {X, X, 0, 2, 2, 2}},
    {E, Q::Major7, FIXED, {0, 2, 1, 1, 0, 0}},
    {F, Q::Major7, FIXED, {X, X, 3, 2, 1, 0}},
    {G, Q::Major7, FIXED, {3, 2, 0, 0, 0, 2}},
    {G, Q::Major7, M, {3, X, 4, 4, 3, X}},

    {A, Q::Sixth, M, {X, 0, 2, 2, 2, 2}},
    {C, Q::Sixth, FIXED, {X, 3, 2, 2, 1, 0}},
    {D, Q::Sixth, FIXED, {X, X, 0, 2, 0, 2}},
    {E, Q::Sixth, FIXED, {0, 2, 2, 1, 2, 0}},
    {F, Q::Sixth, M, {X, X, 3, 2, 3, 1}},
    {G, Q::Sixth, FIXED, {3, 2, 0, 0, 0, 0}},

    {A, Q::Minor6, M, {X, 0, 2, 2, 1, 2}},
    {D, Q::Minor6, FIXED, {X, X, 0, 2, 0, 1}},
    {E, Q::Minor6, M, {0, 2, 2, 0, 2, 0}},

    {A, Q::Ninth, FIXED, {X, 0, 2, 4, 2, 3}},
    {C, Q::Ninth, M, {X, 3, 2, 3, 3, 3}},
    {E, Q::Ninth, M, {0, 2, 0, 1, 0, 2}},
    {G, Q::Ninth, FIXED, {3, X, 0, 2, 0, 1}},

    {A, Q::Add9, M, {X, 0, 2, 4, 2, 0}},
    {C, Q::Add9, FIXED, {X, 3, 2, 0, 3, 0}},
    {D, Q::Add9, FIXED, {X, 5, 4, 2, 3, 0}},
    {E, Q::Add9, M, {0, 2, 4, 1, 0, 0}},
    {G, Q::Add9, FIXED, {3, 2, 0, 2, 0, 3}},

    {A, Q::Sus2, M, {X, 0, 2, 2, 0, 0}},
    {C, Q::Sus2, FIXED, {X, 3, 0, 0, 3, 3}},
    {D, Q::Sus2, M, {X, X, 0, 2, 3, 0}},
    {F, Q::Sus2, FIXED, {X, X, 3, 0, 1, 1}},
    {G, Q::Sus2, FIXED, {3, 0, 0, 0, 3, 3}},

    {A, Q::Sus4, M, {X, 0, 2, 2, 3, 0}},
    {C, Q::Sus4, FIXED, {X, 3, 3, 0, 1, 1}},
    {D, Q::Sus4, FIXED, {X, X, 0, 2, 3, 3}},
    {E, Q::Sus4, M, {0, 2, 2, 2, 0, 0}},
    {G, Q::Sus4, FIXED, {3, 3, 0, 0, 1, 3}},

    {A, Q::Dominant7Sus4, M, {X, 0, 2, 0, 3, 0}},
    {D, Q::Dominant7Sus4, FIXED, {X, X, 0, 2, 1, 3}},
    {E, Q::Dominant7Sus4, M, {0, 2, 0, 2, 0, 0}},

    {B, Q::Diminished, M, {X, 2, 3, 4, 3, X}},
    {D, Q::Diminished, FIXED, {X, X, 0, 1, 3, 1}},
    {G, Q::Diminished, M, {3, 4, 5, 3, X, X}},

    {C, Q::Diminished7, M, {X, 3, 4, 2, 4, X}},
    {D, Q::Diminished7, M, {X, X, 0, 1, 0, 1}},
    {G, Q::Diminished7, M, {3, X, 2, 3, 2, X}},

    {C, Q::Augmented, M, {X, 3, 2, 1, 1, X}},
    {D, Q::Augmented, M, {X, X, 0, 3, 3, 2}},
    {E, Q::Augmented, FIXED, {0, X, 2, 1, 1, 0}},
    {G, Q::Augmented, M, {3, X, 5, 4, 4, X}},
    {A, Q::Augmented, FIXED, {X, 0, 3, 2, 2, 1}},

    {B, Q::HalfDiminished, M, {X, 2, 3, 2, 3, X}},
    {D, Q::HalfDiminished, M, {X, X, 0, 1, 1, 1}},
    {E, Q::HalfDiminished, FIXED, {0, 1, 2, 0, 3, 0}},
    {G, Q::HalfDiminished, M, {3, X, 3, 3, 2, X}},
};

// Strings G C E A (re-entrant, so the lowest string need not be the root).
// Baritone uses these shapes too.
constexpr Shape UKULELE[] = {
    {C, Q::Major, M, {0, 0, 0, 3}},
    {D, Q::Major, FIXED, {2, 2, 2, 0}},
    {Eb, Q::Major, FIXED, {0, 3, 3, 1}},
    {E, Q::Major, FIXED, {4, 4, 4, 2}},
    {F, Q::Major, M, {2, 0, 1, 0}},
    {G, Q::Major, M, {0, 2, 3, 2}},
    {A, Q::Major, M, {2, 1, 0, 0}},

    {A, Q::Minor, M, {2, 0, 0, 0}},
    {C, Q::Minor, M, {0, 3, 3, 3}},
    {D, Q::Minor, M, {2, 2, 1, 0}},
    {E, Q::Minor, FIXED, {0, 4, 3, 2}},
    {F, Q::Minor, FIXED, {1, 0, 1, 3}},
    {Gb, Q::Minor, FIXED, {2, 1, 2, 0}},
    {G, Q::Minor, M, {0, 2, 3, 1}},

    {A, Q::Dominant7, M, {0, 1, 0, 0}},
    {B, Q::Dominant7, FIXED, {2, 3, 2, 2}},
    {C, Q::Dominant7, M, {0, 0, 0, 1}},
    {D, Q::Dominant7, FIXED, {2, 2, 2, 3}},
    {E, Q::Dominant7, FIXED, {1, 2, 0, 2}},
    {F, Q::Dominant7, FIXED, {2, 3, 1, 3}},
    {Gb, Q::Dominant7, FIXED, {3, 4, 2, 4}},
    {G, Q::Dominant7, M, {0, 2, 1, 2}},

    {A, Q::Minor7, M, {0, 0, 0, 0}},
    {C, Q::Minor7, FIXED, {3, 3, 3, 3}},
    {D, Q::Minor7, M, {2, 2, 1, 3}},
    {E, Q::Minor7, FIXED, {0, 2, 0, 2}},
    {F, Q::Minor7, FIXED, {1, 3, 1, 3}},
    {Gb, Q::Minor7, FIXED, {2, 4, 2, 4}},
    {G, Q::Minor7, M, {0, 2, 1, 1}},

    {A, Q::Major7, M, {1, 1, 0, 0}},
    {C, Q::Major7, M, {0, 0, 0, 2}},
    {D, Q::Major7, FIXED, {2, 2, 2, 4}},
    {E, Q::Major7, M, {1, 3, 0, 2}},
    {F, Q::Major7, FIXED, {5, 5, 0, 0}},
    {G, Q::Major7, M, {0, 2, 2, 2}},

    {A, Q::Sixth, M, {2, 4, 2, 4}},
    {C, Q::Sixth, M, {0, 0, 0, 0}},
    {D, Q::Sixth, FIXED, {2, 2, 2, 2}},
    {F, Q::Sixth, M, {2, 2, 1, 3}},
    {G, Q::Sixth, M, {0, 2, 0, 2}},
    {Bb, Q::Sixth, M, {0, 2, 1, 1}},

    {A, Q::Minor6, M, {2, 4, 2, 3}},
    {C, Q::Minor6, M, {2, 3, 3, 3}},
    {D, Q::Minor6, M, {2, 2, 1, 2}},
    {E, Q::Minor6, FIXED, {0, 1, 0, 2}},
    {F, Q::Minor6, M, {1, 2, 1, 3}},
    {G, Q::Minor6, FIXED, {0, 2, 0, 1}},

    // Four strings cannot hold all five notes, so these leave out the root or the fifth.
    {C, Q::Ninth, M, {0, 2, 0, 1}},
    {E, Q::Ninth, M, {1, 2, 2, 2}},
    {G, Q::Ninth, M, {2, 2, 1, 2}},
    {A, Q::Ninth, FIXED, {0, 1, 0, 2}},
    {Bb, Q::Ninth, M, {1, 2, 1, 3}},

    {A, Q::Add9, M, {2, 1, 0, 2}},
    {C, Q::Add9, M, {0, 2, 0, 3}},
    {E, Q::Add9, M, {1, 4, 2, 2}},
    {F, Q::Add9, M, {0, 0, 1, 0}},
    {G, Q::Add9, FIXED, {0, 2, 5, 2}},

    {A, Q::Sus2, FIXED, {2, 4, 0, 2}},
    {C, Q::Sus2, M, {0, 2, 3, 3}},
    {D, Q::Sus2, M, {2, 2, 0, 0}},
    {F, Q::Sus2, M, {0, 0, 1, 3}},
    {G, Q::Sus2, M, {0, 2, 3, 0}},
    {Bb, Q::Sus2, M, {3, 0, 1, 1}},

    {A, Q::Sus4, M, {2, 2, 0, 0}},
    {C, Q::Sus4, M, {0, 0, 1, 3}},
    {D, Q::Sus4, M, {0, 2, 3, 0}},
    {E, Q::Sus4, FIXED, {4, 4, 0, 0}},
    {F, Q::Sus4, M, {3, 0, 1, 1}},
    {G, Q::Sus4, M, {0, 2, 3, 3}},

    {A, Q::Dominant7Sus4, M, {0, 2, 0, 0}},
    {C, Q::Dominant7Sus4, M, {0, 0, 1, 1}},
    {D, Q::Dominant7Sus4, FIXED, {2, 2, 3, 3}},
    {E, Q::Dominant7Sus4, FIXED, {2, 2, 0, 2}},
    {F, Q::Dominant7Sus4, M, {3, 3, 1, 3}},
    {G, Q::Dominant7Sus4, M, {0, 2, 1, 3}},

    {B, Q::Diminished, M, {4, 2, 1, 2}},
    {E, Q::Diminished, M, {0, 4, 0, 1}},
    {G, Q::Diminished, M, {0, 1, 3, 1}},

    // Diminished 7th and augmented chords repeat every 3 and 4 frets, so a few
    // shapes serve every root without moving up the neck.
    {C, Q::Diminished7, FIXED, {2, 3, 2, 3}},
    {Eb, Q::Diminished7, FIXED, {2, 3, 2, 3}},
    {Gb, Q::Diminished7, FIXED, {2, 3, 2, 3}},
    {A, Q::Diminished7, FIXED, {2, 3, 2, 3}},
    {Db, Q::Diminished7, FIXED, {0, 1, 0, 1}},
    {E, Q::Diminished7, FIXED, {0, 1, 0, 1}},
    {G, Q::Diminished7, FIXED, {0, 1, 0, 1}},
    {Bb, Q::Diminished7, FIXED, {0, 1, 0, 1}},
    {D, Q::Diminished7, FIXED, {1, 2, 1, 2}},
    {F, Q::Diminished7, FIXED, {1, 2, 1, 2}},
    {Ab, Q::Diminished7, FIXED, {1, 2, 1, 2}},
    {B, Q::Diminished7, FIXED, {1, 2, 1, 2}},

    {C, Q::Augmented, FIXED, {1, 0, 0, 3}},
    {E, Q::Augmented, FIXED, {1, 0, 0, 3}},
    {Ab, Q::Augmented, FIXED, {1, 0, 0, 3}},
    {D, Q::Augmented, FIXED, {3, 2, 2, 1}},
    {Gb, Q::Augmented, FIXED, {3, 2, 2, 1}},
    {Bb, Q::Augmented, FIXED, {3, 2, 2, 1}},
    {Db, Q::Augmented, FIXED, {2, 1, 1, 0}},
    {F, Q::Augmented, FIXED, {2, 1, 1, 0}},
    {A, Q::Augmented, FIXED, {2, 1, 1, 0}},
    {Eb, Q::Augmented, FIXED, {0, 3, 3, 2}},
    {G, Q::Augmented, FIXED, {0, 3, 3, 2}},
    {B, Q::Augmented, FIXED, {0, 3, 3, 2}},

    {A, Q::HalfDiminished, M, {2, 3, 3, 3}},
    {B, Q::HalfDiminished, M, {2, 2, 1, 2}},
    {D, Q::HalfDiminished, M, {1, 2, 1, 3}},
    {E, Q::HalfDiminished, FIXED, {0, 2, 0, 1}},
    {F, Q::HalfDiminished, M, {1, 3, 1, 2}},
};

constexpr uint8_t GUITAR_TUNING[] = {E, A, D, G, B, E};
constexpr uint8_t UKULELE_TUNING[] = {G, C, E, A};
constexpr uint8_t BARITONE_TUNING[] = {D, G, B, E};
constexpr uint8_t BASS_TUNING[] = {E, A, D, G};
constexpr uint8_t BASS5_TUNING[] = {B, E, A, D, G};
static_assert(std::size(GUITAR_TUNING) <= MAX_STRINGS && std::size(UKULELE_TUNING) <= MAX_STRINGS &&
              std::size(BARITONE_TUNING) <= MAX_STRINGS && std::size(BASS_TUNING) <= MAX_STRINGS &&
              std::size(BASS5_TUNING) <= MAX_STRINGS);
// A bass box starts on one of the strings below the top two, which leaves at
// least three strings: enough for every pitch within five frets.
constexpr uint8_t BASS_TOP_STRINGS = 2;
// Baritone strings are a fourth below the ukulele's, so a ukulele shape for
// the chord a fourth up sounds the wanted chord.
constexpr uint8_t BARITONE_TO_UKULELE = 5;

struct ShapeTable {
  const Shape* shapes;
  size_t count;
};

ShapeTable shapeTable(const Instrument instrument) {
  switch (instrument) {
    case Instrument::Guitar:
      return {GUITAR, std::size(GUITAR)};
    case Instrument::Ukulele:
    case Instrument::Baritone:
      return {UKULELE, std::size(UKULELE)};
    case Instrument::Bass:
    case Instrument::Bass5:
      break;
  }
  return {nullptr, 0};
}

// Sets the window for the frets already in out. A barre shape starts at its
// barre, as chord charts draw it.
void finish(ChordDiagram& out) {
  int highest = 0;
  int lowest = 0;
  for (uint8_t s = 0; s < out.stringCount; ++s) {
    const int fret = out.frets[s];
    if (fret <= 0) continue;
    highest = std::max(highest, fret);
    lowest = lowest == 0 ? fret : std::min(lowest, fret);
  }
  const bool atNut = (out.barreFret == 0 || lowest <= 1) && highest <= DIAGRAM_FRETS;
  out.firstFret = atNut ? 1 : static_cast<uint8_t>(lowest);
}

// Shifted copy of shape; false when the shift would leave the neck.
bool shifted(const Shape& shape, const uint8_t strings, const int shift, ChordDiagram& out) {
  out = ChordDiagram{};
  out.stringCount = strings;
  int firstOpen = -1;
  int lastOpen = -1;
  for (uint8_t s = 0; s < strings; ++s) {
    const int fret = shape.frets[s];
    if (fret == MUTED) continue;
    if (fret == 0) {
      // Open strings cannot move down the neck.
      if (shift < 0) return false;
      if (firstOpen < 0) firstOpen = s;
      lastOpen = s;
    }
    if (fret + shift < 0) return false;
    out.frets[s] = static_cast<int8_t>(fret + shift);
  }
  // A barre replaces the open strings; it cannot pass a muted string.
  if (shift > 0 && lastOpen > firstOpen) {
    bool blocked = false;
    for (int s = firstOpen; s <= lastOpen; ++s) blocked |= shape.frets[s] == MUTED;
    if (!blocked) {
      out.barreFret = static_cast<uint8_t>(shift);
      out.barreFrom = static_cast<uint8_t>(firstOpen);
      out.barreTo = static_cast<uint8_t>(lastOpen);
    }
  }
  return true;
}

int highestFret(const ChordDiagram& diagram) {
  int highest = 0;
  for (uint8_t s = 0; s < diagram.stringCount; ++s) highest = std::max<int>(highest, diagram.frets[s]);
  return highest;
}

bool findInTable(const ShapeTable table, const uint8_t strings, const uint8_t root, const ChordQuality quality,
                 ChordDiagram& out) {
  for (size_t i = 0; i < table.count; ++i) {
    const Shape& shape = table.shapes[i];
    if (shape.quality == quality && shape.root == root) return shifted(shape, strings, 0, out);
  }
  // Otherwise the movable shape that lands lowest on the neck.
  bool found = false;
  int bestHighest = 0;
  for (size_t i = 0; i < table.count; ++i) {
    const Shape& shape = table.shapes[i];
    if (!shape.movable || shape.quality != quality) continue;
    const int up = (root - shape.root + 12) % 12;
    for (const int shift : {up, up - 12}) {
      ChordDiagram candidate;
      if (!shifted(shape, strings, shift, candidate)) continue;
      const int highest = highestFret(candidate);
      if (found && highest >= bestHighest) continue;
      found = true;
      bestHighest = highest;
      out = candidate;
    }
  }
  return found;
}

}  // namespace

const char* instrumentCode(const Instrument instrument) {
  switch (instrument) {
    case Instrument::Guitar:
      return "guitar";
    case Instrument::Ukulele:
      return "ukulele";
    case Instrument::Baritone:
      return "baritone";
    case Instrument::Bass:
      return "bass";
    case Instrument::Bass5:
      return "bass5";
  }
  return "guitar";
}

bool parseInstrumentCode(const std::string_view code, Instrument& out) {
  for (size_t i = 0; i < INSTRUMENT_COUNT; ++i) {
    const auto instrument = static_cast<Instrument>(i);
    if (code != instrumentCode(instrument)) continue;
    out = instrument;
    return true;
  }
  return false;
}

bool isBass(const Instrument instrument) {
  switch (instrument) {
    case Instrument::Guitar:
    case Instrument::Ukulele:
    case Instrument::Baritone:
      return false;
    case Instrument::Bass:
    case Instrument::Bass5:
      return true;
  }
  return false;
}

uint8_t stringCount(const Instrument instrument) {
  switch (instrument) {
    case Instrument::Guitar:
      return std::size(GUITAR_TUNING);
    case Instrument::Ukulele:
      return std::size(UKULELE_TUNING);
    case Instrument::Baritone:
      return std::size(BARITONE_TUNING);
    case Instrument::Bass:
      return std::size(BASS_TUNING);
    case Instrument::Bass5:
      return std::size(BASS5_TUNING);
  }
  return 0;
}

uint8_t openPitch(const Instrument instrument, const uint8_t string) {
  switch (instrument) {
    case Instrument::Guitar:
      return GUITAR_TUNING[string];
    case Instrument::Ukulele:
      return UKULELE_TUNING[string];
    case Instrument::Baritone:
      return BARITONE_TUNING[string];
    case Instrument::Bass:
      return BASS_TUNING[string];
    case Instrument::Bass5:
      return BASS5_TUNING[string];
  }
  return 0;
}

bool findChordDiagram(const ChordName& chord, const Instrument instrument, ChordDiagram& out) {
  switch (instrument) {
    case Instrument::Guitar:
    case Instrument::Ukulele:
      if (!findInTable(shapeTable(instrument), stringCount(instrument), chord.root, chord.quality, out)) return false;
      finish(out);
      return true;
    case Instrument::Baritone: {
      const auto ukuleleRoot = static_cast<uint8_t>((chord.root + BARITONE_TO_UKULELE) % 12);
      if (!findInTable(shapeTable(instrument), stringCount(instrument), ukuleleRoot, chord.quality, out)) return false;
      finish(out);
      return true;
    }
    case Instrument::Bass:
    case Instrument::Bass5:
      break;
  }
  return false;
}

bool findToneMap(const ChordName& chord, const Instrument instrument, ToneMap& out) {
  if (!isBass(instrument)) return false;
  out = ToneMap{};
  out.stringCount = stringCount(instrument);
  const int bassNote = chord.bass != NO_BASS ? chord.bass : chord.root;

  // Start on the low string where the bass note has the lowest fret; a tie keeps the lower string.
  uint8_t startString = 0;
  int startFret = 12;
  for (uint8_t s = 0; s + BASS_TOP_STRINGS < out.stringCount; ++s) {
    const int fret = (bassNote - openPitch(instrument, s) + 12) % 12;
    if (fret >= startFret) continue;
    startFret = fret;
    startString = s;
  }
  // One fret below the bass note, so a major third on the next string fits.
  const int lowest = startFret - 1;
  out.firstFret = static_cast<uint8_t>(lowest <= 1 ? 1 : lowest);
  const bool atNut = out.firstFret == 1;

  const uint16_t tones = chordIntervals(chord.quality);
  const auto classify = [&](const uint8_t string, const int fret, uint8_t& toneBits, uint8_t& bassBits,
                            const uint8_t bit) {
    // Nothing sounds under the bass note: lower strings stay empty, and so do
    // the start string's frets below it.
    if (string == startString && fret < startFret) return;
    const int pitch = (openPitch(instrument, string) + fret) % 12;
    const bool isTone = (tones & 1u << ((pitch - chord.root + 12) % 12)) != 0;
    if (pitch == bassNote) bassBits |= bit;
    if (isTone || pitch == bassNote) toneBits |= bit;
  };
  for (uint8_t s = startString; s < out.stringCount; ++s) {
    for (uint8_t row = 0; row < DIAGRAM_FRETS; ++row) {
      classify(s, out.firstFret + row, out.tones[s], out.bassNotes[s], static_cast<uint8_t>(1u << row));
    }
    if (atNut) classify(s, 0, out.openTones, out.openBassNotes, static_cast<uint8_t>(1u << s));
  }
  return true;
}

}  // namespace songbook
