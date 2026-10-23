#include <gtest/gtest.h>

#include "ChordName.h"

using songbook::ChordName;
using songbook::ChordQuality;
using songbook::NO_BASS;
using songbook::parseChordName;

namespace {

ChordName parsed(const char* text) {
  ChordName chord;
  EXPECT_TRUE(parseChordName(text, chord)) << text;
  return chord;
}

}  // namespace

TEST(ChordName, RootsAndAccidentals) {
  EXPECT_EQ(parsed("C").root, 0);
  EXPECT_EQ(parsed("C#").root, 1);
  EXPECT_EQ(parsed("Db").root, 1);
  EXPECT_EQ(parsed("E").root, 4);
  EXPECT_EQ(parsed("Fb").root, 4);
  EXPECT_EQ(parsed("Cb").root, 11);
  EXPECT_EQ(parsed("B#").root, 0);
  EXPECT_EQ(parsed("Bb").root, 10);
  EXPECT_EQ(parsed("A\xE2\x99\xAF").root, 10);  // A♯
  EXPECT_EQ(parsed("E\xE2\x99\xAD").root, 3);   // E♭
}

TEST(ChordName, HAndBAreBothB) {
  EXPECT_EQ(parsed("H").root, 11);
  EXPECT_EQ(parsed("B").root, 11);
  EXPECT_EQ(parsed("Hm"), parsed("Bm"));
  EXPECT_EQ(parsed("H7").quality, ChordQuality::Dominant7);
}

TEST(ChordName, Qualities) {
  EXPECT_EQ(parsed("A").quality, ChordQuality::Major);
  EXPECT_EQ(parsed("Am").quality, ChordQuality::Minor);
  EXPECT_EQ(parsed("A-").quality, ChordQuality::Minor);
  EXPECT_EQ(parsed("A7").quality, ChordQuality::Dominant7);
  EXPECT_EQ(parsed("Am7").quality, ChordQuality::Minor7);
  EXPECT_EQ(parsed("Amaj7").quality, ChordQuality::Major7);
  EXPECT_EQ(parsed("AM7").quality, ChordQuality::Major7);
  EXPECT_EQ(parsed("A\xCE\x94").quality, ChordQuality::Major7);  // AΔ
  EXPECT_EQ(parsed("A6").quality, ChordQuality::Sixth);
  EXPECT_EQ(parsed("Am6").quality, ChordQuality::Minor6);
  EXPECT_EQ(parsed("A9").quality, ChordQuality::Ninth);
  EXPECT_EQ(parsed("Aadd9").quality, ChordQuality::Add9);
  EXPECT_EQ(parsed("Asus2").quality, ChordQuality::Sus2);
  EXPECT_EQ(parsed("Asus4").quality, ChordQuality::Sus4);
  EXPECT_EQ(parsed("Asus").quality, ChordQuality::Sus4);
  EXPECT_EQ(parsed("A7sus4").quality, ChordQuality::Dominant7Sus4);
  EXPECT_EQ(parsed("Adim").quality, ChordQuality::Diminished);
  EXPECT_EQ(parsed("A\xC2\xB0").quality, ChordQuality::Diminished);  // A°
  EXPECT_EQ(parsed("Adim7").quality, ChordQuality::Diminished7);
  EXPECT_EQ(parsed("Aaug").quality, ChordQuality::Augmented);
  EXPECT_EQ(parsed("A+").quality, ChordQuality::Augmented);
  EXPECT_EQ(parsed("Am7b5").quality, ChordQuality::HalfDiminished);
  EXPECT_EQ(parsed("A\xC3\xB8").quality, ChordQuality::HalfDiminished);   // Aø
  EXPECT_EQ(parsed("A\316\2247").quality, ChordQuality::Major7);          // AΔ7
  EXPECT_EQ(parsed("A\302\2607").quality, ChordQuality::Diminished7);     // A°7
  EXPECT_EQ(parsed("A\303\2707").quality, ChordQuality::HalfDiminished);  // Aø7
}

TEST(ChordName, SlashBass) {
  const ChordName chord = parsed("Am/G");
  EXPECT_EQ(chord.root, 9);
  EXPECT_EQ(chord.quality, ChordQuality::Minor);
  EXPECT_EQ(chord.bass, 7);
  EXPECT_EQ(parsed("D/F#").bass, 6);
  EXPECT_EQ(parsed("C").bass, NO_BASS);
}

TEST(ChordName, TrimsSpaces) { EXPECT_EQ(parsed(" F#m "), parsed("F#m")); }

TEST(ChordName, RejectsNonChords) {
  ChordName chord;
  for (const char* text : {"", "N.C.", "x2", "|", "Solo", "am", "C13", "C7#9", "C/", "C/X", "C/Em", "Cmm"}) {
    EXPECT_FALSE(parseChordName(text, chord)) << text;
  }
}

TEST(ChordName, IntervalsHoldTheRoot) {
  for (size_t i = 0; i < songbook::CHORD_QUALITY_COUNT; ++i) {
    EXPECT_TRUE(songbook::chordIntervals(static_cast<ChordQuality>(i)) & 1u) << i;
  }
  EXPECT_EQ(songbook::chordIntervals(ChordQuality::Minor7), 1u << 0 | 1u << 3 | 1u << 7 | 1u << 10);
}
