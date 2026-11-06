#include <gtest/gtest.h>

#include <expat.h>

#include <string>

#include "Fb2Passes.h"

namespace {

namespace fp = fb2passes;

fp::StructurePass runStructure(const std::string& doc) {
  XML_Parser p = XML_ParserCreate(nullptr);
  EXPECT_NE(p, nullptr);
  fp::StructurePass s;
  XML_SetElementHandler(p, fp::structStart, fp::structEnd);
  XML_SetUserData(p, &s);
  EXPECT_EQ(XML_Parse(p, doc.data(), static_cast<int>(doc.size()), 1), XML_STATUS_OK)
      << XML_ErrorString(XML_GetErrorCode(p));
  XML_ParserFree(p);
  return s;
}

}  // namespace

TEST(Fb2Structure, ChaptersForBodiesAndNotes) {
  const auto s = runStructure(
      "<FictionBook>"
      "<description/>"
      "<body><title/><section id=\"s1\"/><section id=\"s2\"/></body>"       // ch 0 (front), 1, 2
      "<body name=\"notes\"><title/><section id=\"n1\"/><section id=\"n2\"/>"
      "<section id=\"n3\"/></body>"                                          // ch 3 (notes head), 4, 5, 6
      "</FictionBook>");
  // front-matter slot (0) + 2 sections; notes head slot (3) + 3 sections.
  EXPECT_EQ(s.chapterCursor, 6);
  EXPECT_EQ(s.bodyCount, 2);
  EXPECT_EQ(s.idTargets.size(), 5u);
  EXPECT_EQ(s.findIdChapter("s1"), 1);
  EXPECT_EQ(s.findIdChapter("s2"), 2);
  EXPECT_EQ(s.findIdChapter("n1"), 4);
  EXPECT_EQ(s.findIdChapter("n3"), 6);
}

TEST(Fb2Structure, NestedSectionsStayInTheTopLevelChapter) {
  const auto s = runStructure(
      "<FictionBook><body>"
      "<section id=\"part1\"><section id=\"ch1a\"/><section id=\"ch1b\"/></section>"
      "<section id=\"part2\"/>"
      "</body></FictionBook>");
  EXPECT_EQ(s.chapterCursor, 2);  // front slot + part1 + part2
  EXPECT_EQ(s.findIdChapter("part1"), 1);
  EXPECT_EQ(s.findIdChapter("ch1a"), 1);  // nested ids resolve to their top-level chapter
  EXPECT_EQ(s.findIdChapter("ch1b"), 1);
  EXPECT_EQ(s.findIdChapter("part2"), 2);
}

TEST(Fb2Structure, BinaryRegistryByContentType) {
  const auto s = runStructure(
      "<FictionBook><body><section/></body>"
      "<binary id=\"cover.jpg\" content-type=\"image/jpeg\">Zm9v</binary>"
      "<binary id=\"pic\" content-type=\"image/png\">YmFy</binary>"
      "<binary id=\"clip\" content-type=\"video/mp4\">eA==</binary>"
      "</FictionBook>");
  ASSERT_EQ(s.binaries.size(), 3u);
  EXPECT_EQ(s.findBinary("cover.jpg"), 0);
  EXPECT_STREQ(s.binaries[0].ext, ".jpg");
  EXPECT_STREQ(s.binaries[1].ext, ".png");
  EXPECT_STREQ(s.binaries[2].ext, "");  // unsupported -> empty ext, emission skips it
}

TEST(Fb2Structure, DuplicateIdFirstWins) {
  const auto s = runStructure(
      "<FictionBook><body><section id=\"dup\"/><section id=\"dup\"/></body></FictionBook>");
  ASSERT_EQ(s.idTargets.size(), 1u);
  EXPECT_EQ(s.findIdChapter("dup"), 1);
}

TEST(Fb2Structure, EmptyBodyStillCounts) {
  const auto s = runStructure("<FictionBook><body/></FictionBook>");
  EXPECT_EQ(s.chapterCursor, 0);  // the front-matter slot, no sections
}
