#include <gtest/gtest.h>

#include <algorithm>
#include <string>

#include "ChordName.h"
#include "ChordShapes.h"

using songbook::ChordDiagram;
using songbook::ChordName;
using songbook::ChordQuality;
using songbook::DIAGRAM_FRETS;
using songbook::Instrument;
using songbook::MUTED;

namespace {

constexpr Instrument CHORD_INSTRUMENTS[] = {Instrument::Guitar, Instrument::Ukulele, Instrument::Baritone};

std::string describe(const Instrument instrument, const ChordName& chord) {
  return std::string(songbook::instrumentCode(instrument)) + " root " + std::to_string(chord.root) + " quality " +
         std::to_string(static_cast<int>(chord.quality)) + " bass " + std::to_string(chord.bass);
}

std::string describe(const Instrument instrument, const ChordName& chord, const ChordDiagram& diagram) {
  std::string text = describe(instrument, chord) + ":";
  for (uint8_t s = 0; s < diagram.stringCount; ++s) {
    text += ' ';
    text += diagram.frets[s] == MUTED ? "x" : std::to_string(diagram.frets[s]);
  }
  return text;
}

ChordDiagram diagramFor(const char* name, const Instrument instrument) {
  ChordName chord;
  EXPECT_TRUE(songbook::parseChordName(name, chord)) << name;
  ChordDiagram diagram;
  EXPECT_TRUE(songbook::findChordDiagram(chord, instrument, diagram)) << name;
  return diagram;
}

std::string frets(const ChordDiagram& diagram) {
  std::string text;
  for (uint8_t s = 0; s < diagram.stringCount; ++s) {
    text += diagram.frets[s] == MUTED ? "x" : std::to_string(diagram.frets[s]);
  }
  return text;
}

// Pitch-class mask of the sounding strings, relative to root.
uint16_t soundingIntervals(const Instrument instrument, const uint8_t root, const ChordDiagram& diagram) {
  uint16_t mask = 0;
  for (uint8_t s = 0; s < diagram.stringCount; ++s) {
    if (diagram.frets[s] == MUTED) continue;
    mask |= 1u << ((songbook::openPitch(instrument, s) + diagram.frets[s] - root + 24) % 12);
  }
  return mask;
}

// Every shape must fit the drawn window, and a barre must be playable.
void expectDrawable(const ChordDiagram& diagram, const std::string& context) {
  int lowest = 0;
  int highest = 0;
  for (uint8_t s = 0; s < diagram.stringCount; ++s) {
    const int fret = diagram.frets[s];
    if (fret <= 0) continue;
    highest = std::max(highest, fret);
    lowest = lowest == 0 ? fret : std::min(lowest, fret);
  }
  if (diagram.firstFret == 1) {
    EXPECT_LE(highest, DIAGRAM_FRETS) << context;
  } else {
    EXPECT_GE(lowest, diagram.firstFret) << context;
    EXPECT_LT(highest - diagram.firstFret, DIAGRAM_FRETS) << context;
  }
  if (diagram.barreFret == 0) return;
  EXPECT_LT(diagram.barreFrom, diagram.barreTo) << context;
  EXPECT_EQ(diagram.frets[diagram.barreFrom], diagram.barreFret) << context;
  EXPECT_EQ(diagram.frets[diagram.barreTo], diagram.barreFret) << context;
  for (int s = diagram.barreFrom; s <= diagram.barreTo; ++s) {
    EXPECT_GE(diagram.frets[s], diagram.barreFret) << context;
  }
}

}  // namespace

// Checks every shape against music theory: each sounding note belongs to the
// chord, and every note but the fifth is present.
TEST(ChordShapes, EveryChordIsCorrectAndDrawable) {
  for (const Instrument instrument : CHORD_INSTRUMENTS) {
    const uint8_t strings = songbook::stringCount(instrument);
    for (uint8_t root = 0; root < 12; ++root) {
      for (size_t q = 0; q < songbook::CHORD_QUALITY_COUNT; ++q) {
        ChordName chord;
        chord.root = root;
        chord.quality = static_cast<ChordQuality>(q);
        ChordDiagram diagram;
        ASSERT_TRUE(songbook::findChordDiagram(chord, instrument, diagram)) << describe(instrument, chord, diagram);
        const std::string context = describe(instrument, chord, diagram);
        ASSERT_EQ(diagram.stringCount, strings) << context;

        const uint16_t tones = songbook::chordIntervals(chord.quality);
        const uint16_t sounding = soundingIntervals(instrument, root, diagram);
        uint16_t required = tones & ~(1u << 7);
        // Four strings cannot hold a full ninth chord; the root may go.
        if (strings == 4 && chord.quality == ChordQuality::Ninth) required &= ~1u;
        EXPECT_EQ(sounding & ~tones, 0) << context << " has a foreign note";
        EXPECT_EQ(sounding & required, required) << context << " misses a chord tone";

        int soundingStrings = 0;
        int lowestString = -1;
        for (uint8_t s = 0; s < strings; ++s) {
          if (diagram.frets[s] == MUTED) continue;
          ++soundingStrings;
          if (lowestString < 0) lowestString = s;
        }
        EXPECT_GE(soundingStrings, instrument == Instrument::Guitar ? 4 : 3) << context;
        if (instrument == Instrument::Guitar) {
          const int bassNote = (songbook::openPitch(instrument, lowestString) + diagram.frets[lowestString]) % 12;
          EXPECT_EQ(bassNote, root) << context << " is inverted";
        }
        expectDrawable(diagram, context);
      }
    }
  }
}

namespace {

// Checks a bass tone map: every mark is a chord tone or the bass note, every
// chord tone appears, and the box starts at the bass note.
void expectToneMap(const Instrument instrument, const ChordName& chord, const std::string& context) {
  songbook::ToneMap map;
  ASSERT_TRUE(songbook::findToneMap(chord, instrument, map)) << context;
  ChordDiagram unused;
  EXPECT_FALSE(songbook::findChordDiagram(chord, instrument, unused)) << context;
  ASSERT_EQ(map.stringCount, songbook::stringCount(instrument)) << context;
  EXPECT_GE(map.firstFret, 1) << context;
  if (map.openTones != 0) {
    EXPECT_EQ(map.firstFret, 1) << context;
  }

  const int bassNote = chord.bass != songbook::NO_BASS ? chord.bass : chord.root;
  const uint16_t tones = songbook::chordIntervals(chord.quality);
  uint16_t seen = 0;
  int firstString = -1;
  bool bassOnFirstString = false;
  const auto check = [&](const uint8_t string, const int fret, const bool marked, const bool bass) {
    const int pitch = (songbook::openPitch(instrument, string) + fret) % 12;
    const bool isTone = (tones & 1u << ((pitch - chord.root + 12) % 12)) != 0;
    const bool isBassNote = pitch == bassNote;
    EXPECT_EQ(bass, marked && isBassNote) << context << " string " << int{string} << " fret " << fret;
    // The lowest mark is the bass note: on the first string, nothing comes before it.
    const bool belowBass = (firstString < 0 || string == firstString) && !bassOnFirstString && !isBassNote;
    const bool expected = !belowBass && (isTone || isBassNote);
    EXPECT_EQ(marked, expected) << context << " string " << int{string} << " fret " << fret;
    if (!marked) return;
    if (isTone) seen |= 1u << ((pitch - chord.root + 12) % 12);
    if (firstString < 0) firstString = string;
    if (string == firstString && bass) bassOnFirstString = true;
  };
  for (uint8_t s = 0; s < map.stringCount; ++s) {
    const auto bit = static_cast<uint8_t>(1u << s);
    const bool used = firstString >= 0 || map.tones[s] != 0 || (map.openTones & bit) != 0;
    if (!used) continue;
    if (map.firstFret == 1) check(s, 0, (map.openTones & bit) != 0, (map.openBassNotes & bit) != 0);
    for (uint8_t row = 0; row < songbook::DIAGRAM_FRETS; ++row) {
      const auto rowBit = static_cast<uint8_t>(1u << row);
      check(s, map.firstFret + row, (map.tones[s] & rowBit) != 0, (map.bassNotes[s] & rowBit) != 0);
    }
  }
  EXPECT_EQ(seen, tones) << context << " misses a chord tone";
  EXPECT_TRUE(bassOnFirstString) << context << " does not start at the bass note";
}

}  // namespace

TEST(ChordShapes, BassShowsEveryChordTone) {
  for (const Instrument instrument : {Instrument::Bass, Instrument::Bass5}) {
    ASSERT_TRUE(songbook::isBass(instrument));
    for (uint8_t root = 0; root < 12; ++root) {
      for (size_t q = 0; q < songbook::CHORD_QUALITY_COUNT; ++q) {
        ChordName chord;
        chord.root = root;
        chord.quality = static_cast<ChordQuality>(q);
        expectToneMap(instrument, chord, describe(instrument, chord));
        // A slash bass note, inside the chord (the third, for most) and outside it.
        for (const int offset : {4, 2}) {
          chord.bass = static_cast<int8_t>((root + offset) % 12);
          expectToneMap(instrument, chord, describe(instrument, chord));
        }
      }
    }
  }
  for (const Instrument instrument : CHORD_INSTRUMENTS) EXPECT_FALSE(songbook::isBass(instrument));
}

TEST(ChordShapes, KnownShapes) {
  // The barre chord from the reference picture.
  const ChordDiagram bm = diagramFor("Bm", Instrument::Guitar);
  EXPECT_EQ(frets(bm), "x24432");
  EXPECT_EQ(bm.firstFret, 2);
  EXPECT_EQ(bm.barreFret, 2);
  EXPECT_EQ(bm.barreFrom, 1);
  EXPECT_EQ(bm.barreTo, 5);

  const ChordDiagram e = diagramFor("E", Instrument::Guitar);
  EXPECT_EQ(frets(e), "022100");
  EXPECT_EQ(e.firstFret, 1);
  EXPECT_EQ(e.barreFret, 0);
  EXPECT_EQ(frets(diagramFor("D", Instrument::Guitar)), "xx0232");
  EXPECT_EQ(frets(diagramFor("F", Instrument::Guitar)), "133211");
  EXPECT_EQ(frets(diagramFor("Hm", Instrument::Guitar)), "x24432");

  EXPECT_EQ(frets(diagramFor("C", Instrument::Ukulele)), "0003");
  const ChordDiagram bb = diagramFor("Bb", Instrument::Ukulele);
  EXPECT_EQ(frets(bb), "3211");
  EXPECT_EQ(bb.barreFret, 1);
  EXPECT_EQ(bb.barreFrom, 2);
  EXPECT_EQ(bb.barreTo, 3);

  EXPECT_EQ(frets(diagramFor("G", Instrument::Baritone)), "0003");
}

namespace {

songbook::ToneMap toneMapFor(const char* name, const Instrument instrument) {
  ChordName chord;
  EXPECT_TRUE(songbook::parseChordName(name, chord)) << name;
  songbook::ToneMap map;
  EXPECT_TRUE(songbook::findToneMap(chord, instrument, map)) << name;
  return map;
}

}  // namespace

TEST(ChordShapes, ToneMapStartsAtTheLowestBassNote) {
  // Worked out by hand, per bass note C..B: the start string and the window's first fret.
  struct Start {
    uint8_t string;
    uint8_t firstFret;
  };
  static constexpr Start FOUR[12] = {{1, 2}, {1, 3}, {1, 4}, {1, 5}, {0, 1}, {0, 1},
                                     {0, 1}, {0, 2}, {0, 3}, {1, 1}, {1, 1}, {1, 1}};
  static constexpr Start FIVE[12] = {{0, 1}, {0, 1}, {0, 2}, {0, 3}, {1, 1}, {1, 1},
                                     {1, 1}, {1, 2}, {1, 3}, {2, 1}, {2, 1}, {0, 1}};
  for (const Instrument instrument : {Instrument::Bass, Instrument::Bass5}) {
    const Start* expected = instrument == Instrument::Bass ? FOUR : FIVE;
    for (uint8_t note = 0; note < 12; ++note) {
      ChordName chord;
      chord.root = note;
      songbook::ToneMap map;
      ASSERT_TRUE(songbook::findToneMap(chord, instrument, map));
      const std::string context = describe(instrument, chord);
      EXPECT_EQ(map.firstFret, expected[note].firstFret) << context;
      int firstString = -1;
      for (uint8_t s = 0; s < map.stringCount && firstString < 0; ++s) {
        if (map.tones[s] != 0 || (map.openTones & 1u << s) != 0) firstString = s;
      }
      EXPECT_EQ(firstString, expected[note].string) << context;
    }
  }
}

TEST(ChordShapes, KnownToneMaps) {
  // G on a 4-string bass: the box starts at G on the E string, fret 3.
  const auto g = toneMapFor("G", Instrument::Bass);
  EXPECT_EQ(g.firstFret, 2);
  EXPECT_EQ(g.bassNotes[0], 1u << 1);
  // A starts on the open A string; the E string stays empty.
  const auto a = toneMapFor("A", Instrument::Bass);
  EXPECT_EQ(a.firstFret, 1);
  EXPECT_EQ(a.openBassNotes, 1u << 1);
  EXPECT_EQ(a.tones[0], 0);
  EXPECT_EQ(a.openTones & 1u, 0u);
  // On a 5-string bass, C starts on the low B string, fret 1.
  const auto c = toneMapFor("C", Instrument::Bass5);
  EXPECT_EQ(c.firstFret, 1);
  EXPECT_EQ(c.bassNotes[0], 1u << 0);
}

TEST(ChordShapes, InstrumentCodesRoundTrip) {
  for (size_t i = 0; i < songbook::INSTRUMENT_COUNT; ++i) {
    const auto instrument = static_cast<Instrument>(i);
    Instrument parsed = Instrument::Guitar;
    ASSERT_TRUE(songbook::parseInstrumentCode(songbook::instrumentCode(instrument), parsed)) << i;
    EXPECT_EQ(parsed, instrument);
  }
  Instrument parsed = Instrument::Bass;
  EXPECT_FALSE(songbook::parseInstrumentCode("banjo", parsed));
  EXPECT_FALSE(songbook::parseInstrumentCode("", parsed));
  EXPECT_EQ(parsed, Instrument::Bass);
  EXPECT_STREQ(songbook::instrumentCode(Instrument::Ukulele), "ukulele");
}

TEST(ChordShapes, SlashChords) {
  // Chord instruments show the upper chord; bass starts at the bass note.
  EXPECT_EQ(frets(diagramFor("Am/G", Instrument::Guitar)), frets(diagramFor("Am", Instrument::Guitar)));
  const auto ce = toneMapFor("C/E", Instrument::Bass);
  EXPECT_EQ(ce.openBassNotes, 1u << 0);
  const auto dfs = toneMapFor("D/F#", Instrument::Bass);
  EXPECT_EQ(dfs.firstFret, 1);
  EXPECT_EQ(dfs.bassNotes[0], 1u << 1);
}
