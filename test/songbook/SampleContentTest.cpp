#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

#include "ChordShapes.h"
#include "Setlist.h"
#include "SongChords.h"
#include "SongbookTestSupport.h"

namespace {

std::string readSample(const std::string& relative) {
  std::ifstream file(std::string(SONGBOOK_SAMPLE_DIR) + "/" + relative, std::ios::binary);
  std::stringstream content;
  content << file.rdbuf();
  return content.str();
}

}  // namespace

TEST(SongbookSamples, SetlistSongsParse) {
  songbook::Setlist setlist;
  ASSERT_EQ(songbook::parseSetlist(readSample("setlists/concert.json"), setlist), songbook::SetlistParseResult::Ok);
  ASSERT_GE(setlist.songs.size(), 2u);
  for (const auto& path : setlist.songs) {
    const std::string source = readSample("songs/" + path);
    ASSERT_FALSE(source.empty()) << path;
    const songbook::Song song = songbook_test::parse(source);
    EXPECT_FALSE(song.meta.title.empty()) << path;
    EXPECT_FALSE(song.meta.key.empty()) << path;
    EXPECT_GT(song.meta.durationSec, 0u) << path;
    EXPECT_FALSE(song.sections.empty()) << path;
    EXPECT_FALSE(song.chords.empty()) << path;
    songbook::SongLayout lyrics;
    songbook::SongLayout chart;
    const songbook_test::FixedMeasurer measurer;
    lyrics.buildLyrics(song, measurer, songbook::Viewport{488, 660}, songbook::SectionNames{});
    chart.buildChart(song, measurer, songbook::Viewport{488, 660}, songbook::SectionNames{});
    EXPECT_FALSE(lyrics.truncated() || chart.truncated()) << path;
    const size_t budget =
        songbook::SongLayout::lyricsBudget(song).bytes() + songbook::SongLayout::chartBudget(song).bytes();
    EXPECT_LT(budget, 24u * 1024u) << path << " needs " << budget;

    // Every chord of a shipped song has a diagram on every instrument.
    songbook::SongChords chords;
    chords.collect(song);
    EXPECT_FALSE(chords.truncated()) << path;
    for (size_t i = 0; i < chords.size(); ++i) {
      const std::string name(song.view(chords[i].name));
      ASSERT_TRUE(chords[i].known) << path << ": " << name;
      for (size_t n = 0; n < songbook::INSTRUMENT_COUNT; ++n) {
        const auto instrument = static_cast<songbook::Instrument>(n);
        songbook::ChordDiagram diagram;
        songbook::ToneMap tones;
        const bool found = songbook::isBass(instrument)
                               ? songbook::findToneMap(chords[i].chord, instrument, tones)
                               : songbook::findChordDiagram(chords[i].chord, instrument, diagram);
        EXPECT_TRUE(found) << path << ": " << name << " on " << songbook::instrumentCode(instrument);
      }
    }
  }
}
