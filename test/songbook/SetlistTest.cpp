#include <gtest/gtest.h>

#include "Setlist.h"

using songbook::Setlist;
using songbook::SetlistParseResult;

TEST(Setlist, ParsesTitleAndOrderedSongs) {
  Setlist setlist;
  ASSERT_EQ(songbook::parseSetlist(R"({"title": "Концерт", "songs": ["b.cho", "a.cho", "rock/c.chordpro"]})", setlist),
            SetlistParseResult::Ok);
  EXPECT_EQ(setlist.title, "Концерт");
  ASSERT_EQ(setlist.songs.size(), 3u);
  EXPECT_EQ(setlist.songs[0], "b.cho");
  EXPECT_EQ(setlist.songs[2], "rock/c.chordpro");
}

TEST(Setlist, RejectsCorruptJson) {
  Setlist setlist;
  EXPECT_EQ(songbook::parseSetlist(R"({"title": "x", "songs": ["a.cho")", setlist), SetlistParseResult::InvalidJson);
  EXPECT_EQ(songbook::parseSetlist("", setlist), SetlistParseResult::InvalidJson);
  EXPECT_EQ(songbook::parseSetlist(R"(["a.cho"])", setlist), SetlistParseResult::InvalidJson);
  EXPECT_EQ(songbook::parseSetlist(R"({"songs": "a.cho"})", setlist), SetlistParseResult::InvalidJson);
}

TEST(Setlist, SkipsUnsafeAndNonStringEntries) {
  Setlist setlist;
  ASSERT_EQ(songbook::parseSetlist(
                R"({"songs": ["../x.cho", "/abs.cho", "a//b.cho", "a\\b.cho", 5, null, {"f": 1}, "ok.cho"]})", setlist),
            SetlistParseResult::Ok);
  ASSERT_EQ(setlist.songs.size(), 1u);
  EXPECT_EQ(setlist.songs[0], "ok.cho");
  EXPECT_TRUE(setlist.title.empty());
}

TEST(Setlist, EmptySongListIsReported) {
  Setlist setlist;
  EXPECT_EQ(songbook::parseSetlist(R"({"title": "Empty", "songs": []})", setlist), SetlistParseResult::NoSongs);
  EXPECT_EQ(setlist.title, "Empty");
}

TEST(Setlist, CapsSongCount) {
  std::string json = R"({"songs": [)";
  for (size_t i = 0; i < songbook::MAX_SETLIST_SONGS + 20; ++i)
    json += (i ? ",\"" : "\"") + std::to_string(i) + ".cho\"";
  json += "]}";
  Setlist setlist;
  ASSERT_EQ(songbook::parseSetlist(json, setlist), SetlistParseResult::Ok);
  EXPECT_EQ(setlist.songs.size(), songbook::MAX_SETLIST_SONGS);
}

TEST(Setlist, SafePathRules) {
  EXPECT_TRUE(songbook::isSafeSongPath("song.cho"));
  EXPECT_TRUE(songbook::isSafeSongPath("folder/песня.cho"));
  EXPECT_FALSE(songbook::isSafeSongPath(""));
  EXPECT_FALSE(songbook::isSafeSongPath("/song.cho"));
  EXPECT_FALSE(songbook::isSafeSongPath("../song.cho"));
  EXPECT_FALSE(songbook::isSafeSongPath("a/../b.cho"));
  EXPECT_FALSE(songbook::isSafeSongPath("./a.cho"));
  EXPECT_FALSE(songbook::isSafeSongPath("dir/"));
  EXPECT_FALSE(songbook::isSafeSongPath("a\nb.cho"));
}

TEST(Setlist, SongExtensions) {
  EXPECT_TRUE(songbook::hasSongExtension("a.cho"));
  EXPECT_TRUE(songbook::hasSongExtension("A.CHORDPRO"));
  EXPECT_TRUE(songbook::hasSongExtension("a.chopro"));
  EXPECT_FALSE(songbook::hasSongExtension("a.txt"));
  EXPECT_FALSE(songbook::hasSongExtension("cho"));
}
