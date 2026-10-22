#include <gtest/gtest.h>

#include <string>

#include "SongbookTestSupport.h"

using songbook::ChordProParser;
using songbook::LineKind;
using songbook::SectionType;
using songbook::Song;
using songbook_test::parse;

namespace {

std::string chordName(const Song& song, const songbook::SongLine& line, const int index) {
  return std::string(song.view(song.chords[line.firstChord + index].name));
}

std::string lyricFrom(const Song& song, const songbook::SongLine& line, const int chordIndex) {
  return std::string(song.view(line.lyric).substr(song.chords[line.firstChord + chordIndex].position));
}

}  // namespace

TEST(ChordProParser, ReadsMetadata) {
  const Song song =
      parse("{title: Example Song}\n{artist: Example Artist}\n{key: Em}\n{tempo: 100}\n{duration: 210}\n{capo: 2}\n");
  EXPECT_EQ(song.meta.title, "Example Song");
  EXPECT_EQ(song.meta.artist, "Example Artist");
  EXPECT_EQ(song.meta.key, "Em");
  EXPECT_EQ(song.meta.capo, "2");
  EXPECT_EQ(song.meta.tempo, 100);
  EXPECT_EQ(song.meta.durationSec, 210u);
}

TEST(ChordProParser, ShortFormsAndMetaDirective) {
  const Song song = parse("{t:Short}\n{meta: artist Someone}\n{meta: duration 3:30}\n{key Am}\n");
  EXPECT_EQ(song.meta.title, "Short");
  EXPECT_EQ(song.meta.artist, "Someone");
  EXPECT_EQ(song.meta.durationSec, 210u);
  EXPECT_EQ(song.meta.key, "Am");
}

TEST(ChordProParser, ParsesDurationForms) {
  EXPECT_EQ(ChordProParser::parseDuration("210"), 210u);
  EXPECT_EQ(ChordProParser::parseDuration("3:30"), 210u);
  EXPECT_EQ(ChordProParser::parseDuration(" 1:02:03 "), 3723u);
  EXPECT_EQ(ChordProParser::parseDuration("abc"), 0u);
  EXPECT_EQ(ChordProParser::parseDuration("3:"), 0u);
  EXPECT_EQ(ChordProParser::parseDuration(""), 0u);
  EXPECT_EQ(ChordProParser::parseDuration("1:2:3:4"), 0u);
}

TEST(ChordProParser, AnchorsChordsToLyricBytes) {
  const Song song = parse("[Em]First [Am]line of the [D]song");
  ASSERT_EQ(song.lines.size(), 1u);
  const auto& line = song.lines[0];
  EXPECT_EQ(song.view(line.lyric), "First line of the song");
  ASSERT_EQ(line.chordCount, 3);
  EXPECT_EQ(chordName(song, line, 0), "Em");
  EXPECT_EQ(song.chords[line.firstChord].position, 0);
  EXPECT_EQ(lyricFrom(song, line, 1), "line of the song");
  EXPECT_EQ(lyricFrom(song, line, 2), "song");
}

TEST(ChordProParser, AnchorsCyrillicAtUtf8Boundaries) {
  const Song song = parse("[Am]Ой, то не [Dm]вечер, то не [E]вечер");
  ASSERT_EQ(song.lines.size(), 1u);
  const auto& line = song.lines[0];
  EXPECT_EQ(song.view(line.lyric), "Ой, то не вечер, то не вечер");
  ASSERT_EQ(line.chordCount, 3);
  EXPECT_EQ(chordName(song, line, 1), "Dm");
  EXPECT_EQ(lyricFrom(song, line, 1), "вечер, то не вечер");
  EXPECT_EQ(lyricFrom(song, line, 2), "вечер");
}

TEST(ChordProParser, MidWordAndEndOfLineChords) {
  const Song song = parse("Hel[C]lo wor[G7]ld [Am]");
  const auto& line = song.lines[0];
  EXPECT_EQ(song.view(line.lyric), "Hello world ");
  ASSERT_EQ(line.chordCount, 3);
  EXPECT_EQ(lyricFrom(song, line, 0), "lo world ");
  EXPECT_EQ(song.chords[line.firstChord + 2].position, line.lyric.length);
}

TEST(ChordProParser, ChordOnlyAndAnnotationLines) {
  const Song song = parse("[Em] [D] [*Riff]");
  const auto& line = song.lines[0];
  ASSERT_EQ(line.chordCount, 3);
  EXPECT_EQ(chordName(song, line, 2), "Riff");
}

TEST(ChordProParser, SectionsAndRepeats) {
  const Song song = parse(
      "{start_of_verse}\n[Em]Verse line\n{end_of_verse}\n"
      "{soc}\n[C]Chorus line\n{eoc}\n"
      "{start_of_bridge: Middle 8}\n[F]Bridge\n{end_of_bridge}\n"
      "{start_of_intro}\n[Em] [D]\n{end_of_intro}\n"
      "{start_of_solo}\n[A] [B]\n{end_of_solo}\n"
      "{chorus}\n");
  ASSERT_EQ(song.sections.size(), 6u);
  EXPECT_EQ(song.sections[0].type, SectionType::Verse);
  EXPECT_EQ(song.sections[1].type, SectionType::Chorus);
  EXPECT_EQ(song.sections[2].type, SectionType::Bridge);
  EXPECT_EQ(song.view(song.sections[2].label), "Middle 8");
  EXPECT_EQ(song.sections[3].type, SectionType::Intro);
  EXPECT_EQ(song.sections[4].type, SectionType::Other);
  EXPECT_EQ(song.view(song.sections[4].label), "solo");
  EXPECT_TRUE(song.sections[5].repeat);
  EXPECT_EQ(song.sections[5].repeatOf, 1);
  EXPECT_EQ(song.sections[5].lineCount, 0);
}

TEST(ChordProParser, LabelAttributeAndUnresolvedChorus) {
  const Song song = parse("{chorus}\n{start_of_verse label=\"Куплет 2\"}\nТекст\n{end_of_verse}\n");
  ASSERT_EQ(song.sections.size(), 2u);
  EXPECT_TRUE(song.sections[0].repeat);
  EXPECT_EQ(song.sections[0].repeatOf, -1);
  EXPECT_EQ(song.view(song.sections[1].label), "Куплет 2");
}

TEST(ChordProParser, LinesOutsideSectionsGetImplicitSection) {
  const Song song = parse("[G]Free line\n\n\n[C]Another\n");
  ASSERT_EQ(song.sections.size(), 1u);
  EXPECT_EQ(song.sections[0].type, SectionType::None);
  // Blank runs collapse to one spacer line.
  ASSERT_EQ(song.lines.size(), 3u);
  EXPECT_EQ(song.lines[1].kind, LineKind::Empty);
}

TEST(ChordProParser, TrailingBlankLinesAreDropped) {
  const Song song = parse("{sov}\n\nText\n\n\n{eov}\n");
  ASSERT_EQ(song.sections.size(), 1u);
  EXPECT_EQ(song.sections[0].lineCount, 1);
}

TEST(ChordProParser, CommentsAndSourceComments) {
  const Song song = parse("# not shown\n{c: Guitar solo x2}\n{ci: soft}\n");
  ASSERT_EQ(song.lines.size(), 2u);
  EXPECT_EQ(song.lines[0].kind, LineKind::Comment);
  EXPECT_EQ(song.view(song.lines[0].lyric), "Guitar solo x2");
}

TEST(ChordProParser, UnknownDirectivesAreSkipped) {
  Song song;
  ChordProParser parser(song);
  parser.feedLine("{new_page}");
  parser.feedLine("{x_custom: value}");
  parser.feedLine("{textfont: Times}");
  parser.feedLine("[C]Still parsed");
  parser.finish();
  EXPECT_EQ(parser.skippedDirectives(), 3);
  ASSERT_EQ(song.lines.size(), 1u);
}

TEST(ChordProParser, TabLinesKeepBracketsAndSpacing) {
  const Song song = parse("{sot}\ne|--[0]--3--|\n{eot}\n");
  ASSERT_EQ(song.lines.size(), 1u);
  EXPECT_EQ(song.lines[0].kind, LineKind::Tab);
  EXPECT_EQ(song.view(song.lines[0].lyric), "e|--[0]--3--|");
}

TEST(ChordProParser, ExplicitChart) {
  const Song song =
      parse("{start_of_x_chart}\nIntro: Em D\nVerse: Em Am D G | x2\nC G D\n{end_of_x_chart}\n[Em]Text\n");
  ASSERT_EQ(song.chart.size(), 3u);
  EXPECT_EQ(song.view(song.chart[1].label), "Verse");
  EXPECT_EQ(song.view(song.chart[1].chords), "Em Am D G | x2");
  EXPECT_EQ(song.chart[2].label.length, 0);
  ASSERT_EQ(song.lines.size(), 1u);
}

TEST(ChordProParser, BareCueCopiesNextLyricWords) {
  Song song;
  ChordProParser parser(song);
  parser.feedLine("{soc}");
  parser.feedLine("{x_cue}");
  parser.feedLine("[C] [G]");
  parser.feedLine("[Am]Ах, эта [F]свадьба");
  parser.feedLine("{eoc}");
  parser.finish();
  EXPECT_EQ(parser.skippedDirectives(), 0);
  ASSERT_EQ(song.lines.size(), 3u);
  // A chord-only line has no words, so the cue waits for the next line.
  EXPECT_EQ(song.lines[0].kind, LineKind::Lyrics);
  EXPECT_EQ(song.lines[1].kind, LineKind::Cue);
  EXPECT_EQ(song.lines[1].chordCount, 0);
  EXPECT_EQ(song.view(song.lines[1].lyric), "Ах, эта свадьба");
  EXPECT_EQ(song.lines[2].kind, LineKind::Lyrics);
  EXPECT_EQ(song.lines[2].chordCount, 2);
  EXPECT_EQ(song.sections[0].lineCount, 3);
}

TEST(ChordProParser, CueWithText) {
  const Song song = parse("{sov}\n{x_cue: Second verse words}\n[C]Text\n{eov}\n");
  ASSERT_EQ(song.lines.size(), 2u);
  EXPECT_EQ(song.lines[0].kind, LineKind::Cue);
  EXPECT_EQ(song.view(song.lines[0].lyric), "Second verse words");
  EXPECT_EQ(song.lines[1].kind, LineKind::Lyrics);
}

TEST(ChordProParser, CueWithTextAlsoMarksChordOnlyLines) {
  const Song song = parse("{sob}\n{x_cue: Build up}\n[Am] [G]\n{eob}\n");
  ASSERT_EQ(song.lines.size(), 2u);
  EXPECT_EQ(song.lines[0].kind, LineKind::Cue);
  EXPECT_EQ(song.view(song.lines[0].lyric), "Build up");
}

TEST(ChordProParser, CueBetweenSectionsJoinsTheNextOne) {
  const Song song = parse("{sov}\n[C]a\n{eov}\n{x_cue: Chorus words}\n{soc}\n[F]b\n{eoc}\n");
  ASSERT_EQ(song.sections.size(), 2u);
  EXPECT_EQ(song.sections[1].type, SectionType::Chorus);
  ASSERT_EQ(song.sections[1].lineCount, 2);
  EXPECT_EQ(song.lines[song.sections[1].firstLine].kind, LineKind::Cue);
}

TEST(ChordProParser, CueAfterStrayCommentJoinsTheNextSection) {
  const Song song = parse("{sov}\n[C]a\n{eov}\n{c: Solo}\n{x_cue: Words}\n{sov}\n[C]b\n{eov}\n");
  ASSERT_EQ(song.sections.size(), 3u);
  ASSERT_EQ(song.sections[2].lineCount, 2);
  EXPECT_EQ(song.lines[song.sections[2].firstLine].kind, LineKind::Cue);
  EXPECT_EQ(song.view(song.lines[song.sections[2].firstLine].lyric), "Words");
}

TEST(ChordProParser, CueDoesNotCrossSectionEnd) {
  const Song song = parse("{sov}\n[C]a\n{x_cue}\n[Am] [G]\n{eov}\n{soc}\n[F]b\n{eoc}\n");
  for (const auto& line : song.lines) EXPECT_NE(line.kind, LineKind::Cue);
}

TEST(ChordProParser, CueIsIgnoredInExplicitChartAndMetadata) {
  const Song chart = parse("{start_of_x_chart}\n{x_cue: no}\nVerse: C G\n{end_of_x_chart}\n[C]Text\n");
  ASSERT_EQ(chart.lines.size(), 1u);
  EXPECT_EQ(chart.lines[0].kind, LineKind::Lyrics);
  const Song meta = parse("{title: T}\n{x_cue: no}\n{sov}\n[C]Text\n{eov}\n", true);
  EXPECT_TRUE(meta.lines.empty());
  EXPECT_TRUE(meta.text.empty());
}

TEST(ChordProParser, BareCueStaysWithinLineLimit) {
  Song song;
  ChordProParser parser(song);
  for (size_t i = 0; i + 1 < ChordProParser::MAX_LINES; ++i) parser.feedLine("[C]Line");
  parser.feedLine("{x_cue}");
  parser.feedLine("[C]Last");
  parser.finish();
  EXPECT_EQ(song.lines.size(), ChordProParser::MAX_LINES);
  EXPECT_EQ(song.lines.back().kind, LineKind::Lyrics);
  EXPECT_TRUE(parser.truncated());
}

TEST(ChordProParser, StripsBomAndCarriageReturns) {
  const Song song = parse("\xEF\xBB\xBF{title: BOM}\r\n[C]Line\r\n");
  EXPECT_EQ(song.meta.title, "BOM");
  ASSERT_EQ(song.lines.size(), 1u);
  EXPECT_EQ(song.view(song.lines[0].lyric), "Line");
}

TEST(ChordProParser, MalformedInputIsBounded) {
  const Song song = parse("[Em unclosed bracket\n{title\n}\n{}\n[]\n\x01\x02\xff\xfe garbage\n");
  ASSERT_GE(song.lines.size(), 1u);
  EXPECT_EQ(song.view(song.lines[0].lyric), "[Em unclosed bracket");
  EXPECT_EQ(song.lines[0].chordCount, 0);
}

TEST(ChordProParser, EmptyInput) {
  const Song song = parse("");
  EXPECT_TRUE(song.lines.empty());
  EXPECT_TRUE(song.sections.empty());
}

TEST(ChordProParser, LineCountLimitTruncates) {
  Song song;
  ChordProParser parser(song);
  for (size_t i = 0; i < ChordProParser::MAX_LINES + 50; ++i) parser.feedLine("[C]x");
  parser.finish();
  EXPECT_TRUE(parser.truncated());
  EXPECT_EQ(song.lines.size(), ChordProParser::MAX_LINES);
}

TEST(ChordProParser, OverlongLineIsCut) {
  Song song;
  ChordProParser parser(song);
  parser.feedLine(std::string(ChordProParser::MAX_LINE_BYTES * 2, 'a'));
  parser.finish();
  EXPECT_TRUE(parser.truncated());
  EXPECT_EQ(song.lines[0].lyric.length, ChordProParser::MAX_LINE_BYTES);
}

TEST(ChordProParser, MetadataOnlyStopsAtBody) {
  Song song;
  ChordProParser parser(song, true);
  EXPECT_TRUE(parser.feedLine("{title: Head}"));
  EXPECT_TRUE(parser.feedLine(""));
  EXPECT_TRUE(parser.feedLine("{artist: A}"));
  EXPECT_FALSE(parser.feedLine("{start_of_verse}"));
  parser.finish();
  EXPECT_EQ(song.meta.title, "Head");
  EXPECT_EQ(song.meta.artist, "A");
  EXPECT_TRUE(song.lines.empty());
  EXPECT_TRUE(song.text.empty());
}

TEST(ChordProParser, ExplicitChartRowsAreCapped) {
  Song song;
  ChordProParser parser(song);
  parser.feedLine("{start_of_x_chart}");
  for (size_t i = 0; i < ChordProParser::MAX_CHART_ROWS + 100; ++i) parser.feedLine("V: C G");
  parser.feedLine("{end_of_x_chart}");
  parser.finish();
  EXPECT_TRUE(parser.truncated());
  EXPECT_EQ(song.chart.size(), ChordProParser::MAX_CHART_ROWS);
}

TEST(ChordProParser, MetadataOnlySkipsExplicitChart) {
  Song song;
  ChordProParser parser(song, true);
  parser.feedLine("{title: T}");
  parser.feedLine("{start_of_x_chart}");
  parser.feedLine("Verse: C G");
  parser.feedLine("{end_of_x_chart}");
  parser.finish();
  EXPECT_EQ(song.meta.title, "T");
  EXPECT_TRUE(song.chart.empty());
  EXPECT_TRUE(song.text.empty());
}
