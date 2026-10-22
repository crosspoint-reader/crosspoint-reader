#include <EpdFont.h>
#include <Utf8.h>
#include <builtinFonts/notosans_18_bold.h>
#include <builtinFonts/notosans_18_regular.h>
#include <gtest/gtest.h>

#include <string>

#include "SongbookTestSupport.h"

using songbook::FontSize;
using songbook::Piece;
using songbook::Row;
using songbook::SectionNames;
using songbook::Song;
using songbook::SongLayout;
using songbook::TextRole;
using songbook::Viewport;
using songbook_test::FixedMeasurer;
using songbook_test::parse;

namespace {

const FixedMeasurer measurer;
const SectionNames names;

const Piece* findPiece(const SongLayout& layout, const Song& song, const std::string& text, size_t skip = 0) {
  for (const auto& piece : layout.pieces()) {
    if (layout.text(song, piece) == text && skip-- == 0) return &piece;
  }
  return nullptr;
}

const Piece* findPieceStarting(const SongLayout& layout, const Song& song, const std::string& prefix) {
  for (const auto& piece : layout.pieces()) {
    if (layout.text(song, piece).substr(0, prefix.size()) == prefix) return &piece;
  }
  return nullptr;
}

const Row* rowOf(const SongLayout& layout, const Piece* piece) {
  const size_t index = static_cast<size_t>(piece - layout.pieces().data());
  for (const auto& row : layout.rows()) {
    if (index >= row.firstPiece && index < row.firstPiece + row.pieceCount) return &row;
  }
  return nullptr;
}

std::string longSong(const int lines) {
  std::string source = "{title: Long}\n{sov}\n";
  for (int i = 0; i < lines; ++i) source += "[C]Line number " + std::to_string(i) + "\n";
  source += "{eov}\n";
  return source;
}

}  // namespace

TEST(SongLayout, PlacesChordsAboveTheirWords) {
  const Song song = parse("[Em]Hello [Am]world");
  SongLayout layout;
  layout.buildLyrics(song, measurer, Viewport{400, 600}, names);
  ASSERT_EQ(layout.rows().size(), 1u);
  EXPECT_EQ(layout.rows()[0].height, 26 + 30);
  const Piece* am = findPiece(layout, song, "Am");
  const Piece* world = findPiece(layout, song, "world");
  ASSERT_TRUE(am && world);
  EXPECT_EQ(am->x, 60);  // width("Hello ")
  EXPECT_EQ(world->x, am->x);
  EXPECT_EQ(am->dy, 0);
  EXPECT_EQ(world->dy, 26);
}

TEST(SongLayout, PlacesCyrillicByCodepointWidth) {
  const Song song = parse("[Am]Ой, то не [Dm]вечер");
  SongLayout layout;
  layout.buildLyrics(song, measurer, Viewport{500, 600}, names);
  const Piece* dm = findPiece(layout, song, "Dm");
  const Piece* word = findPiece(layout, song, "вечер");
  ASSERT_TRUE(dm && word);
  EXPECT_EQ(dm->x, 100);  // "Ой, то не " is 10 codepoints
  EXPECT_EQ(word->x, 100);
}

TEST(SongLayout, MergesChordlessWordsIntoOnePiece) {
  const Song song = parse("[C]One two three [G]four five");
  SongLayout layout;
  layout.buildLyrics(song, measurer, Viewport{500, 600}, names);
  // Two chords and two lyric runs, not one piece per word.
  EXPECT_EQ(layout.pieces().size(), 4u);
  EXPECT_TRUE(findPiece(layout, song, "One two three"));
  EXPECT_TRUE(findPiece(layout, song, "four five"));
}

TEST(SongLayout, WideChordPushesFollowingLyric) {
  const Song song = parse("[Cmaj7]A[G]b");
  SongLayout layout;
  layout.buildLyrics(song, measurer, Viewport{400, 600}, names);
  const Piece* g = findPiece(layout, song, "G");
  const Piece* b = findPiece(layout, song, "b");
  ASSERT_TRUE(g && b);
  EXPECT_EQ(g->x, 5 * 12 + 12);  // Cmaj7 width plus one chord space
  EXPECT_EQ(b->x, g->x);
}

TEST(SongLayout, WrapsLongLinesKeepingChordsWithWords) {
  const Song song = parse("[C]one two three four five six seven [G]eight nine ten eleven twelve");
  SongLayout layout;
  constexpr int WIDTH = 200;
  layout.buildLyrics(song, measurer, Viewport{WIDTH, 600}, names);
  EXPECT_GT(layout.rows().size(), 1u);
  for (const auto& piece : layout.pieces()) {
    EXPECT_LE(piece.x + measurer.width(piece.role, piece.size, layout.text(song, piece)), WIDTH);
  }
  const Piece* g = findPiece(layout, song, "G");
  const Piece* eight = findPieceStarting(layout, song, "eight");
  ASSERT_TRUE(g && eight);
  EXPECT_EQ(rowOf(layout, g), rowOf(layout, eight));
  EXPECT_EQ(g->x, eight->x);
}

TEST(SongLayout, ChordOnlyAndLyricOnlyRowsAreCompact) {
  const Song song = parse("[Em] [D]\nJust words");
  SongLayout layout;
  layout.buildLyrics(song, measurer, Viewport{400, 600}, names);
  ASSERT_EQ(layout.rows().size(), 2u);
  EXPECT_EQ(layout.rows()[0].height, 26);
  EXPECT_EQ(layout.rows()[1].height, 30);
  EXPECT_EQ(layout.rows()[1].y, 26);
}

TEST(SongLayout, SectionLabelsAndChorusBar) {
  const Song song = parse("{sov}\n[C]a\n{eov}\n{sov}\n[C]b\n{eov}\n{soc}\n[F]c\n{eoc}\n");
  SongLayout layout;
  layout.buildLyrics(song, measurer, Viewport{400, 600}, names);
  ASSERT_TRUE(findPiece(layout, song, "Verse 1"));
  ASSERT_TRUE(findPiece(layout, song, "Verse 2"));
  const Piece* chorusLine = findPiece(layout, song, "c");
  ASSERT_TRUE(chorusLine);
  EXPECT_TRUE(rowOf(layout, chorusLine)->chorusBar);
  EXPECT_EQ(chorusLine->x, SongLayout::CHORUS_INDENT);
}

TEST(SongLayout, RepeatedChorusIsPrintedAgain) {
  const Song song = parse("{soc}\n[C]Chorus line\n{eoc}\n{sov}\n[G]Verse\n{eov}\n{chorus}\n");
  SongLayout layout;
  layout.buildLyrics(song, measurer, Viewport{400, 600}, names);
  EXPECT_TRUE(findPiece(layout, song, "Chorus line", 1));
  EXPECT_FALSE(findPiece(layout, song, "Chorus line", 2));
}

TEST(SongLayout, ShortSongFitsWithoutScrolling) {
  const Song song = parse(longSong(4));
  SongLayout layout;
  layout.buildLyrics(song, measurer, Viewport{400, 600}, names);
  EXPECT_LE(layout.contentHeight(), 600);
  ASSERT_EQ(layout.scrollStops().size(), 1u);
  EXPECT_EQ(layout.scrollStops()[0], 0);
}

TEST(SongLayout, LongSongStopsAreRowAlignedThirds) {
  const Song song = parse(longSong(200));
  SongLayout layout;
  constexpr int VIEW = 600;
  layout.buildLyrics(song, measurer, Viewport{400, VIEW}, names);
  const auto& stops = layout.scrollStops();
  ASSERT_GT(stops.size(), 10u);
  for (size_t k = 1; k < stops.size(); ++k) {
    EXPECT_GT(stops[k], stops[k - 1]);
    EXPECT_LE(stops[k] - stops[k - 1], VIEW / 3);
    bool aligned = false;
    for (const auto& row : layout.rows()) aligned |= row.y == stops[k];
    EXPECT_TRUE(aligned) << "stop " << k;
  }
  // The final stop shows the last row whole, and the one before does not.
  EXPECT_LE(layout.contentHeight() - stops.back(), VIEW);
  EXPECT_GT(layout.contentHeight() - stops[stops.size() - 2], VIEW);
}

TEST(SongLayout, ChartCollapsesRepeatedLines) {
  const Song song = parse("{sov}\n[Em]a [Am]b [D]c [G]d\n[Em]e [Am]f [D]g [G]h\n{eov}\n");
  SongLayout layout;
  layout.buildChart(song, measurer, Viewport{500, 700}, names);
  EXPECT_TRUE(layout.autoChart());
  EXPECT_EQ(layout.pageCount(), 1);
  EXPECT_EQ(layout.chartSize(), FontSize::Large);
  EXPECT_TRUE(findPiece(layout, song,
                        "\xC3\x97"
                        "2"));
  EXPECT_FALSE(findPiece(layout, song, "Em", 1));
}

TEST(SongLayout, ChartMarksSectionsWithSameChords) {
  const Song song = parse("{sov}\n[Em]a [D]b\n{eov}\n{sov}\n[Em]c [D]d\n{eov}\n{soc}\n[C]x\n{eoc}\n{chorus}\n");
  SongLayout layout;
  layout.buildChart(song, measurer, Viewport{500, 700}, names);
  EXPECT_TRUE(findPiece(layout, song, "Verse 2 (same chords)"));
  EXPECT_TRUE(findPiece(layout, song, "Chorus (repeat)"));
}

TEST(SongLayout, ChartUsesExplicitSummary) {
  const Song song = parse("{start_of_x_chart}\nVerse: Em Am D G | x2\n{end_of_x_chart}\n{sov}\n[F]other\n{eov}\n");
  SongLayout layout;
  layout.buildChart(song, measurer, Viewport{500, 700}, names);
  EXPECT_FALSE(layout.autoChart());
  EXPECT_TRUE(findPiece(layout, song, "Em Am D G | x2"));
  EXPECT_FALSE(findPiece(layout, song, "F"));
}

TEST(SongLayout, ChartUsesTwoColumnsBeforeSmallerFont) {
  std::string source;
  for (int i = 0; i < 8; ++i) {
    source += "{sov}\n[A" + std::to_string(i) + "]x [B" + std::to_string(i) + "]y\n{eov}\n";
  }
  const Song song = parse(source);
  SongLayout layout;
  constexpr int VIEW = 300;
  layout.buildChart(song, measurer, Viewport{500, VIEW}, names);
  EXPECT_EQ(layout.pageCount(), 1);
  EXPECT_EQ(layout.columns(), 2);
  EXPECT_EQ(layout.chartSize(), FontSize::Large);
  bool secondColumn = false;
  for (const auto& row : layout.rows()) {
    EXPECT_LE(row.y + row.height, VIEW);
    secondColumn |= row.x > 0;
  }
  EXPECT_TRUE(secondColumn);
}

TEST(SongLayout, VeryLongChartPaginates) {
  std::string source;
  for (int i = 0; i < 80; ++i) {
    source += "{sov}\n[C" + std::to_string(i) + "]x [D" + std::to_string(i) + "]y\n{eov}\n";
  }
  const Song song = parse(source);
  SongLayout layout;
  constexpr int VIEW = 400;
  layout.buildChart(song, measurer, Viewport{300, VIEW}, names);
  EXPECT_GT(layout.pageCount(), 1);
  EXPECT_EQ(layout.chartSize(), FontSize::Small);
  uint16_t lastPage = 0;
  for (const auto& row : layout.rows()) {
    EXPECT_LE(row.y + row.height, VIEW);
    EXPECT_GE(row.page, lastPage);
    lastPage = row.page;
  }
  EXPECT_EQ(lastPage + 1, layout.pageCount());
}

TEST(SongLayout, ReaderFontsCoverCyrillicAndRepeatMark) {
  const EpdFont regular(&notosans_18_regular);
  const EpdFont bold(&notosans_18_bold);
  const char* sample = "Ой, то не вечер! ЁёЪъЫы Ііїєґ \xC3\x97";
  const auto* cursor = reinterpret_cast<const unsigned char*>(sample);
  while (*cursor) {
    const uint32_t cp = utf8NextCodepoint(&cursor);
    EXPECT_TRUE(regular.hasCodepoint(cp)) << std::hex << cp;
    EXPECT_TRUE(bold.hasCodepoint(cp)) << std::hex << cp;
  }
}

TEST(SongLayout, BudgetBoundsTablesForSongAtParserCaps) {
  // Many short words and dense chords: the worst case for piece and row counts.
  std::string source = "{sov}\n";
  std::string line;
  for (int i = 0; i < 60; ++i) line += "[C]a b ";
  for (size_t i = 0; i < songbook::ChordProParser::MAX_LINES; ++i) source += line + "\n";
  const Song song = parse(source);
  SongLayout lyrics;
  lyrics.buildLyrics(song, measurer, Viewport{480, 650}, names);
  const auto budget = SongLayout::lyricsBudget(song);
  EXPECT_LE(lyrics.rows().size(), budget.rows);
  EXPECT_LE(lyrics.pieces().size(), budget.pieces);
  EXPECT_EQ(lyrics.rows().capacity(), budget.rows);
  EXPECT_EQ(lyrics.pieces().capacity(), budget.pieces);

  SongLayout chart;
  chart.buildChart(song, measurer, Viewport{480, 650}, names);
  const auto chartBudget = SongLayout::chartBudget(song);
  EXPECT_EQ(chart.rows().capacity(), chartBudget.rows);
  EXPECT_EQ(chart.pieces().capacity(), chartBudget.pieces);
}

TEST(SongLayout, NarrowViewportTruncatesInsteadOfGrowing) {
  const Song song = parse(longSong(40));
  SongLayout layout;
  // Every word on its own row overruns the row estimate.
  layout.buildLyrics(song, measurer, Viewport{12, 600}, names);
  const auto budget = SongLayout::lyricsBudget(song);
  EXPECT_TRUE(layout.truncated());
  EXPECT_EQ(layout.rows().size(), budget.rows);
  EXPECT_EQ(layout.rows().capacity(), budget.rows);
  for (const auto& row : layout.rows()) EXPECT_LE(row.firstPiece + row.pieceCount, layout.pieces().size());
}

TEST(SongLayout, TypicalSongBudgetIsSmall) {
  const Song song = parse(longSong(60));
  SongLayout layout;
  layout.buildLyrics(song, measurer, Viewport{480, 650}, names);
  EXPECT_FALSE(layout.truncated());
  EXPECT_LT(SongLayout::lyricsBudget(song).bytes() + SongLayout::chartBudget(song).bytes(), 32u * 1024u);
}

TEST(SongLayout, LongCyrillicLabelsKeepTheWholeSong) {
  std::string source;
  for (int i = 0; i < 30; ++i) {
    source += "{start_of_verse: Очень длинное название куплета номер " + std::to_string(i) + "}\n[Am]Строка\n{eov}\n";
  }
  source += "{sov}\n[E]Последняя строка\n{eov}\n";
  const Song song = parse(source);
  SongLayout lyrics;
  lyrics.buildLyrics(song, measurer, Viewport{480, 650}, names);
  SongLayout chart;
  chart.buildChart(song, measurer, Viewport{480, 650}, names);
  EXPECT_FALSE(lyrics.truncated());
  EXPECT_FALSE(chart.truncated());
  EXPECT_TRUE(findPieceStarting(lyrics, song, "Последняя"));
}

TEST(SongLayout, ChordDenseLinesFitTheRowBudget) {
  std::string source = "{sov}\n";
  for (int line = 0; line < 30; ++line) {
    for (int i = 0; i < 24; ++i) source += "[Cmaj7] ";
    source += "\n";
  }
  source += "{eov}\n";
  const Song song = parse(source);
  SongLayout lyrics;
  lyrics.buildLyrics(song, measurer, Viewport{480, 650}, names);
  SongLayout chart;
  chart.buildChart(song, measurer, Viewport{480, 650}, names);
  EXPECT_FALSE(lyrics.truncated());
  EXPECT_FALSE(chart.truncated());
}

namespace {
// Every piece must end inside its own column (or the full width in one column).
void expectPiecesInsideColumns(const SongLayout& layout, const Song& song, const int width) {
  const int columnWidth = layout.columns() == 2 ? (width - SongLayout::COLUMN_GAP) / 2 : width;
  for (const auto& row : layout.rows()) {
    for (uint32_t i = row.firstPiece; i < row.firstPiece + row.pieceCount; ++i) {
      const auto& piece = layout.pieces()[i];
      EXPECT_LE(piece.x + measurer.width(piece.role, piece.size, layout.text(song, piece)), columnWidth)
          << layout.text(song, piece);
    }
  }
}
}  // namespace

TEST(SongLayout, LongSectionLabelsWrapInsideTwoColumns) {
  std::string source;
  for (int i = 0; i < 8; ++i) {
    source += "{start_of_verse: A very long section name that cannot fit " + std::to_string(i) + "}\n[A" +
              std::to_string(i) + "]x [B" + std::to_string(i) + "]y\n{eov}\n";
  }
  const Song song = parse(source);
  SongLayout chart;
  constexpr int WIDTH = 500;
  chart.buildChart(song, measurer, Viewport{WIDTH, 400}, names);
  ASSERT_EQ(chart.columns(), 2);
  EXPECT_FALSE(chart.truncated());
  expectPiecesInsideColumns(chart, song, WIDTH);
}

TEST(SongLayout, LongExplicitChartLabelsWrap) {
  const Song song =
      parse("{start_of_x_chart}\nПервый очень длинный куплет с подробным названием: Am Dm E Am\n{end_of_x_chart}\n");
  SongLayout chart;
  chart.buildChart(song, measurer, Viewport{300, 700}, names);
  EXPECT_FALSE(chart.truncated());
  expectPiecesInsideColumns(chart, song, 300);
}

TEST(SongLayout, LongLyricsLabelsWrap) {
  const Song song =
      parse("{start_of_verse: Куплет с очень длинным названием, которое не влезает в строку}\n[C]Text\n{eov}\n");
  SongLayout lyrics;
  lyrics.buildLyrics(song, measurer, Viewport{240, 700}, names);
  EXPECT_FALSE(lyrics.truncated());
  expectPiecesInsideColumns(lyrics, song, 240);
}
