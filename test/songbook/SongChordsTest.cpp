#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "ChordShapes.h"
#include "DiagramLayout.h"
#include "SongChords.h"
#include "SongbookTestSupport.h"

using songbook::DiagramLayout;
using songbook::FontSize;
using songbook::SongChords;
using songbook::Viewport;

namespace {

std::vector<std::string> names(const songbook::Song& song, const SongChords& chords) {
  std::vector<std::string> out;
  for (size_t i = 0; i < chords.size(); ++i) out.emplace_back(song.view(chords[i].name));
  return out;
}

using Names = std::vector<std::string>;

// A setlist screen on X3: the viewport the sample test also uses.
constexpr Viewport SCREEN{488, 660};

// The cell rectangle of every chord lies inside the viewport, without overlap on a page.
void expectInside(const DiagramLayout& layout, const size_t count, const Viewport viewport) {
  for (size_t i = 0; i < count; ++i) {
    const auto placement = layout.place(i);
    EXPECT_LT(placement.page, layout.pageCount()) << i;
    EXPECT_GE(placement.cellX, 0) << i;
    EXPECT_GE(placement.cellY, 0) << i;
    EXPECT_LE(placement.cellX + layout.cellWidth(), viewport.width) << i;
    EXPECT_LE(placement.cellY + layout.cellHeight(), viewport.height) << i;
    if (i > 0 && layout.place(i - 1).page == placement.page) {
      const auto previous = layout.place(i - 1);
      EXPECT_TRUE(placement.cellX >= previous.cellX + layout.cellWidth() || placement.cellY > previous.cellY) << i;
    }
  }
}

}  // namespace

TEST(SongChords, DistinctInOrderOfFirstUse) {
  const auto song = songbook_test::parse("[G]One [D]two [G]three\n[Em]four [D]five\n");
  SongChords chords;
  chords.collect(song);
  EXPECT_EQ(names(song, chords), (Names{"G", "D", "Em"}));
  EXPECT_TRUE(chords[0].known);
  EXPECT_FALSE(chords.truncated());
}

TEST(SongChords, SameChordUnderAnotherNameIsShownOnce) {
  const auto song = songbook_test::parse("[A#]one [Bb]two [Hm]three [Bm]four\n");
  SongChords chords;
  chords.collect(song);
  // The first spelling names the diagram.
  EXPECT_EQ(names(song, chords), (Names{"A#", "Hm"}));
}

TEST(SongChords, SlashChordIsItsOwnEntry) {
  const auto song = songbook_test::parse("[C]one [C/E]two\n");
  SongChords chords;
  chords.collect(song);
  EXPECT_EQ(names(song, chords), (Names{"C", "C/E"}));
}

TEST(SongChords, SkipsAnnotationsAndNonChords) {
  const auto song = songbook_test::parse("[*Riff]one [*Am]two [N.C.]three [Bridge - quiet]x [G]four\n");
  SongChords chords;
  chords.collect(song);
  EXPECT_EQ(names(song, chords), (Names{"G"}));
}

TEST(SongChords, KeepsUnknownChordsOnce) {
  const auto song = songbook_test::parse("[C13]one [C13]two [C7#9]three\n");
  SongChords chords;
  chords.collect(song);
  ASSERT_EQ(names(song, chords), (Names{"C13", "C7#9"}));
  EXPECT_FALSE(chords[0].known);
  EXPECT_FALSE(chords[1].known);
}

TEST(SongChords, ReadsExplicitChart) {
  const auto song = songbook_test::parse(
      "[Am]Lyric\n"
      "{start_of_x_chart}\n"
      "Verse: G G7 C G | G Em D\n"
      "Chorus: (C) G,D G | x2\n"
      "{end_of_x_chart}\n");
  SongChords chords;
  chords.collect(song);
  // Labels are not chords; bar lines and "x2" are skipped.
  EXPECT_EQ(names(song, chords), (Names{"Am", "G", "G7", "C", "Em", "D"}));
}

TEST(SongChords, StopsAtTheLimit) {
  static constexpr const char* ROOTS[] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
  static constexpr const char* QUALITIES[] = {"", "m", "7", "m7", "maj7"};
  std::string source;
  size_t total = 0;
  for (const char* quality : QUALITIES) {
    for (const char* root : ROOTS) {
      source += std::string("[") + root + quality + "]x ";
      ++total;
    }
    source += "\n";
  }
  ASSERT_GT(total, SongChords::MAX_CHORDS);
  const auto song = songbook_test::parse(source);
  SongChords chords;
  chords.collect(song);
  EXPECT_EQ(chords.size(), SongChords::MAX_CHORDS);
  EXPECT_TRUE(chords.truncated());
}

TEST(DiagramLayout, FewChordsUseTheLargeSizeOnOnePage) {
  const auto song = songbook_test::parse("[G]a [D]b [Em]c [C]d\n");
  SongChords chords;
  chords.collect(song);
  DiagramLayout layout;
  layout.build(song, chords, songbook::stringCount(songbook::Instrument::Guitar), songbook_test::FixedMeasurer{},
               SCREEN);
  EXPECT_EQ(layout.size(), FontSize::Large);
  EXPECT_EQ(layout.pageCount(), 1);
  expectInside(layout, chords.size(), SCREEN);
  // The grid sits inside its cell, below the name and the open/muted marks.
  const auto first = layout.place(0);
  const auto& metrics = layout.metrics();
  EXPECT_GE(first.gridX - metrics.dotRadius, first.cellX);
  EXPECT_LE(first.gridX + 5 * metrics.stringGap + metrics.dotRadius + metrics.fretLabelWidth,
            first.cellX + layout.cellWidth());
  EXPECT_EQ(first.gridY, first.cellY + metrics.padding + metrics.nameHeight + metrics.markHeight);
}

TEST(DiagramLayout, ManyChordsShrinkThenUsePages) {
  std::string source;
  for (const char* root : {"C", "D", "E", "F", "G", "A", "B"}) {
    for (const char* quality : {"", "m", "7", "m7", "maj7", "sus4"})
      source += std::string("[") + root + quality + "]x ";
    source += "\n";
  }
  const auto song = songbook_test::parse(source);
  SongChords chords;
  chords.collect(song);
  ASSERT_EQ(chords.size(), 42u);
  DiagramLayout layout;
  layout.build(song, chords, songbook::stringCount(songbook::Instrument::Guitar), songbook_test::FixedMeasurer{},
               SCREEN);
  EXPECT_EQ(layout.size(), FontSize::Small);
  EXPECT_GT(layout.pageCount(), 1);
  EXPECT_EQ(layout.place(layout.perPage()).page, 1);
  EXPECT_EQ(layout.place(layout.perPage()).cellY, 0);
  expectInside(layout, chords.size(), SCREEN);
}

TEST(DiagramLayout, FourStringsFitMoreColumns) {
  const auto song = songbook_test::parse("[G]a\n");
  SongChords chords;
  chords.collect(song);
  DiagramLayout guitar;
  DiagramLayout ukulele;
  const songbook_test::FixedMeasurer measurer;
  guitar.build(song, chords, 6, measurer, SCREEN);
  ukulele.build(song, chords, 4, measurer, SCREEN);
  EXPECT_GT(ukulele.columns(), guitar.columns());
}

TEST(DiagramLayout, LongChordNameWidensTheCell) {
  const auto shortSong = songbook_test::parse("[G]a\n");
  const auto longSong = songbook_test::parse("[C#m7b5/G#]a\n");
  SongChords shortChords;
  SongChords longChords;
  shortChords.collect(shortSong);
  longChords.collect(longSong);
  const songbook_test::FixedMeasurer measurer;
  DiagramLayout narrow;
  DiagramLayout wide;
  narrow.build(shortSong, shortChords, 4, measurer, SCREEN);
  wide.build(longSong, longChords, 4, measurer, SCREEN);
  EXPECT_GT(wide.cellWidth(), narrow.cellWidth());
  EXPECT_GE(wide.cellWidth(), measurer.width(songbook::TextRole::Chord, wide.size(), "C#m7b5/G#"));
}

TEST(DiagramLayout, UnknownNameDoesNotWidenTheCell) {
  // Text in brackets that is not a chord must not squeeze every other diagram.
  const auto plain = songbook_test::parse("[G]a [D]b\n");
  const auto noisy = songbook_test::parse("[G]a [D]b [Gmaj7sus4add11/F#]c\n");
  SongChords plainChords;
  SongChords noisyChords;
  plainChords.collect(plain);
  noisyChords.collect(noisy);
  ASSERT_EQ(noisyChords.size(), 3u);
  const songbook_test::FixedMeasurer measurer;
  DiagramLayout expected;
  DiagramLayout layout;
  expected.build(plain, plainChords, 6, measurer, SCREEN);
  layout.build(noisy, noisyChords, 6, measurer, SCREEN);
  EXPECT_EQ(layout.size(), expected.size());
  EXPECT_EQ(layout.cellWidth(), expected.cellWidth());
}

TEST(DiagramLayout, FretNumberAndMarksStayInTheCell) {
  const auto song = songbook_test::parse("[G]a [Bm]b\n");
  SongChords chords;
  chords.collect(song);
  const songbook_test::FixedMeasurer measurer;
  for (const uint8_t strings : {uint8_t{4}, uint8_t{6}}) {
    for (const Viewport viewport : {SCREEN, Viewport{150, 200}}) {
      DiagramLayout layout;
      layout.build(song, chords, strings, measurer, viewport);
      const auto& metrics = layout.metrics();
      const auto at = layout.place(1);
      const int labelWidth = measurer.width(songbook::TextRole::Label, layout.size(), "12");
      EXPECT_LE(at.fretLabelX + labelWidth, at.cellX + layout.cellWidth()) << int{strings};
      EXPECT_GE(at.markY - metrics.markRadius, at.cellY + metrics.padding + metrics.nameHeight) << int{strings};
      EXPECT_LE(at.markY + metrics.markRadius, at.gridY) << int{strings};
      EXPECT_LE(at.gridY + songbook::DIAGRAM_FRETS * metrics.fretGap, at.cellY + layout.cellHeight()) << int{strings};
    }
  }
}

TEST(DiagramLayout, TinyViewportStillPlacesEveryChord) {
  const auto song = songbook_test::parse("[G]a [D]b [Em]c\n");
  SongChords chords;
  chords.collect(song);
  DiagramLayout layout;
  layout.build(song, chords, 6, songbook_test::FixedMeasurer{}, Viewport{10, 10});
  EXPECT_EQ(layout.perPage(), 1u);
  EXPECT_EQ(layout.pageCount(), 3);
  EXPECT_EQ(layout.place(2).page, 2);
}
