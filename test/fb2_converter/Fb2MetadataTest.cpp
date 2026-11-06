#include <gtest/gtest.h>

#include <expat.h>

#include <string>

#include "Fb2Passes.h"

namespace {

// Drives the real handlers through a real expat parser over an in-memory FB2
// document — the same flow Fb2Converter::readMetadata uses (including the
// early-stop on <body>), without Storage.
namespace fp = fb2passes;

fp::Metadata runMeta(const std::string& doc, bool* stoppedEarly = nullptr) {
  XML_Parser p = XML_ParserCreate(nullptr);
  EXPECT_NE(p, nullptr);
  if (p == nullptr) return {};
  fp::MetaPass m;
  m.parser = p;
  XML_SetElementHandler(p, fp::metaStart, fp::metaEnd);
  XML_SetCharacterDataHandler(p, fp::metaText);
  XML_SetUserData(p, &m);
  const bool ok = XML_Parse(p, doc.data(), static_cast<int>(doc.size()), 1) != XML_STATUS_ERROR;
  const bool aborted = !ok && XML_GetErrorCode(p) == XML_ERROR_ABORTED;
  XML_ParserFree(p);
  if (stoppedEarly) *stoppedEarly = aborted;
  return m.collect();
}

}  // namespace

TEST(Fb2Metadata, FullDescription) {
  const auto meta = runMeta(
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?><FictionBook>"
      "<description><title-info>"
      "<genre>prose_contemporary</genre>"
      "<author><first-name>Виктор</first-name><last-name>Пелевин</last-name></author>"
      "<book-title>Непобедимое солнце. Книга 1</book-title>"
      "<annotation><p>Аннотация</p></annotation>"
      "<date>2021</date>"
      "<coverpage><image l:href=\"#cover.jpg\"/></coverpage>"
      "<lang>ru</lang>"
      "<sequence name=\"Непобедимое солнце\" number=\"1\"/>"
      "</title-info></description>"
      "<body><section><p>Текст</p></section></body></FictionBook>");
  EXPECT_EQ(meta.title, "Непобедимое солнце. Книга 1");
  EXPECT_EQ(meta.author, "Виктор Пелевин");
  EXPECT_EQ(meta.language, "ru");
  EXPECT_EQ(meta.seriesName, "Непобедимое солнце");
  EXPECT_EQ(meta.seriesNumber, "1");
  EXPECT_EQ(meta.coverRef, "cover.jpg");
}

TEST(Fb2Metadata, NicknameOnlyAuthor) {
  const auto meta = runMeta(
      "<FictionBook><description><title-info>"
      "<author><nickname>nick</nickname></author>"
      "<book-title>T</book-title><lang>en</lang>"
      "</title-info></description><body/></FictionBook>");
  EXPECT_EQ(meta.author, "nick");
}

TEST(Fb2Metadata, FirstAuthorAndSequenceWin) {
  const auto meta = runMeta(
      "<FictionBook><description><title-info>"
      "<author><first-name>A</first-name></author>"
      "<author><first-name>B</first-name></author>"
      "<book-title>T</book-title>"
      "<sequence name=\"S1\" number=\"2\"/><sequence name=\"S2\" number=\"9\"/>"
      "</title-info></description><body/></FictionBook>");
  EXPECT_EQ(meta.author, "A");
  EXPECT_EQ(meta.seriesName, "S1");
  EXPECT_EQ(meta.seriesNumber, "2");
}

TEST(Fb2Metadata, DocumentInfoAuthorIgnored) {
  // document-info (after title-info) has its own <author> — must not overwrite.
  const auto meta = runMeta(
      "<FictionBook><description><title-info>"
      "<author><first-name>Reader</first-name><last-name>Author</last-name></author>"
      "<book-title>T</book-title>"
      "</title-info>"
      "<document-info><author><nickname>scanner</nickname></author></document-info>"
      "</description><body/></FictionBook>");
  EXPECT_EQ(meta.author, "Reader Author");
}

TEST(Fb2Metadata, MissingTitleInfoLeavesEverythingEmpty) {
  const auto meta = runMeta(
      "<FictionBook><description><publish-info><book-name>x</book-name></publish-info></description>"
      "<body><section><p>t</p></section></body></FictionBook>");
  EXPECT_TRUE(meta.title.empty());
  EXPECT_TRUE(meta.author.empty());
  EXPECT_TRUE(meta.seriesName.empty());
  EXPECT_TRUE(meta.coverRef.empty());
}

TEST(Fb2Metadata, EarlyStopsAtBody) {
  bool early = false;
  const auto meta = runMeta(
      "<FictionBook><description><title-info>"
      "<book-title>Only</book-title><lang>ru</lang>"
      "</title-info></description>"
      "<body><section><p>ЭТОНЕДОЛЖНОПОПАСТЬ</p></section></body>"
      "<binary id=\"b\" content-type=\"image/jpeg\">AAAA</binary></FictionBook>",
      &early);
  EXPECT_TRUE(early);  // XML_StopParser fired at <body>
  EXPECT_EQ(meta.title, "Only");
  EXPECT_EQ(meta.language, "ru");
}
