#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ChordName.h"

namespace songbook {

// Soprano, concert, and tenor ukuleles share the GCEA tuning, so one entry covers them.
// Bass5 (B E A D G) stays last; INSTRUMENT_COUNT counts up to it.
enum class Instrument : uint8_t { Guitar, Ukulele, Baritone, Bass, Bass5 };
constexpr size_t INSTRUMENT_COUNT = 5;
static_assert(static_cast<size_t>(Instrument::Bass5) + 1 == INSTRUMENT_COUNT);

constexpr uint8_t MAX_STRINGS = 6;
// Fret rows a diagram draws; every shape fits in them.
constexpr uint8_t DIAGRAM_FRETS = 5;
constexpr int8_t MUTED = -1;

struct ChordDiagram {
  uint8_t stringCount = 0;
  // Fret of the top row; 1 means the row under the nut.
  uint8_t firstFret = 1;
  // Absolute fret per string, in drawn order (left to right): MUTED, 0 = open, or fretted.
  int8_t frets[MAX_STRINGS] = {MUTED, MUTED, MUTED, MUTED, MUTED, MUTED};
  // 0 = no barre; otherwise the barre covers strings barreFrom..barreTo.
  uint8_t barreFret = 0;
  uint8_t barreFrom = 0;
  uint8_t barreTo = 0;
};

// Every chord tone within five frets, for bass lines rather than strumming.
// Several tones may share a string.
struct ToneMap {
  uint8_t stringCount = 0;
  // Fret of the top row; 1 means the row under the nut.
  uint8_t firstFret = 1;
  // Per string, bit r marks a tone on fret firstFret + r.
  uint8_t tones[MAX_STRINGS] = {};
  // The same bits for the bass note: the root, or the note after '/'.
  uint8_t bassNotes[MAX_STRINGS] = {};
  // Bit per string: the open string is a tone, or the bass note.
  uint8_t openTones = 0;
  uint8_t openBassNotes = 0;
};

// Stable names for saved settings; never persist the enum value.
const char* instrumentCode(Instrument instrument);
bool parseInstrumentCode(std::string_view code, Instrument& out);

// Bass instruments show a ToneMap instead of a ChordDiagram.
bool isBass(Instrument instrument);
uint8_t stringCount(Instrument instrument);
// Pitch class of an open string, in drawn order (G C E A on ukulele).
uint8_t openPitch(Instrument instrument, uint8_t string);

// Fills out with a playable shape for a chord instrument. A slash chord shows
// its upper chord. False for a bass or when no shape is known.
bool findChordDiagram(const ChordName& chord, Instrument instrument, ChordDiagram& out);

// Fills out for a bass. The box starts one fret below the bass note on one of
// the low strings, and nothing under the bass note is marked. The note after
// '/' joins the chord tones. False for a chord instrument.
bool findToneMap(const ChordName& chord, Instrument instrument, ToneMap& out);

}  // namespace songbook
