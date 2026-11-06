#include <gtest/gtest.h>

#include <expat.h>

#include <cstring>

#include "Fb2Encoding.h"

namespace {

// Runs the unknown-encoding handler through a real expat parser: the declared
// encoding is unknown to expat, so it must consult our tables.
void appendText(void* ud, const XML_Char* s, int len) {
  static_cast<std::string*>(ud)->append(s, len);
}

std::string parseWithEncoding(const std::string& decl, const char* bodyBytes) {
  XML_Parser p = XML_ParserCreate(nullptr);
  XML_SetUnknownEncodingHandler(p, fb2UnknownEncodingHandler, nullptr);
  XML_SetCharacterDataHandler(p, appendText);
  std::string out;
  XML_SetUserData(p, &out);
  const std::string doc = std::string(decl) + "<r>" + bodyBytes + "</r>";
  const bool ok = XML_Parse(p, doc.data(), static_cast<int>(doc.size()), 1) != XML_STATUS_ERROR;
  const auto err = XML_GetErrorCode(p);
  XML_ParserFree(p);
  if (!ok) return std::string("PARSE_ERROR:") + XML_ErrorString(err);
  return out;
}

}  // namespace

TEST(Fb2Encoding, Cp1251CyrillicDecodesToUtf8) {
  // 0xC0 0xC1 in cp1251 = АБ -> UTF-8 D0 90 D0 91
  EXPECT_EQ(parseWithEncoding(R"(<?xml version="1.0" encoding="windows-1251"?>)", "\xC0\xC1"),
            "\xD0\x90\xD0\x91");
  // я = 0xFF -> D1 8F; Ё = 0xA8 -> D0 81; № = 0xB9 -> E2 84 96
  EXPECT_EQ(parseWithEncoding(R"(<?xml version="1.0" encoding="windows-1251"?>)", "\xFF\xA8\xB9"),
            "\xD1\x8F\xD0\x81\xE2\x84\x96");
}

TEST(Fb2Encoding, Koi8RClassicPangramLetters) {
  // KOI8-R maps by transliteration sound: 0xE1 = А (U+0410), 0xF7 = В (U+0412).
  EXPECT_EQ(parseWithEncoding(R"(<?xml version="1.0" encoding="koi8-r"?>)", "\xE1\xF7"),
            "\xD0\x90\xD0\x92");  // АВ
}

TEST(Fb2Encoding, Latin1AndCp1252Names) {
  // Both aliases route to their tables; 0xE9 = é (00E9) in latin-1.
  EXPECT_EQ(parseWithEncoding(R"(<?xml version="1.0" encoding="iso-8859-1"?>)", "\xE9"), "\xC3\xA9");
  // cp1252 0x80 = € (20AC)
  EXPECT_EQ(parseWithEncoding(R"(<?xml version="1.0" encoding="cp1252"?>)", "\x80"), "\xE2\x82\xAC");
}

TEST(Fb2Encoding, NameComparisonIgnoresCaseAndSeparators) {
  EXPECT_TRUE(fb2EncodingNameEquals("Windows-1251", "windows1251"));
  EXPECT_TRUE(fb2EncodingNameEquals("KOI8_R", "koi8-r"));
  EXPECT_FALSE(fb2EncodingNameEquals("windows-1251", "koi8-r"));
  EXPECT_FALSE(fb2EncodingNameEquals("utf-8", "utf16"));  // different tails
}

TEST(Fb2Encoding, UnknownEncodingStillRejected) {
  // Anything we don't map must fail the parse, not produce mojibake.
  EXPECT_TRUE(parseWithEncoding(R"(<?xml version="1.0" encoding="iso-8859-5"?>)", "\xC0")
                  .rfind("PARSE_ERROR", 0) == 0);
}
