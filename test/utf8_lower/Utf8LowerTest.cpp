// utf8LowerCodepoint, the one lowercase mapping the hyphenators and the
// dictionary lookup share, utf8ToLower, its whole-word form, and
// utf8UpperCodepoint, its inverse (small caps).
//
// Every block it covers is a few arithmetic rules rather than a table, so the
// cases pin each rule's ends and the exceptions that break a block's pattern.
// The mapping was checked against Python's str.lower() for every codepoint
// below U+2000 when the Greek and Cyrillic rules were added.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "Utf8.h"

namespace {

TEST(Utf8Lower, LatinBlocks) {
  EXPECT_EQ(utf8LowerCodepoint('A'), static_cast<uint32_t>('a'));
  EXPECT_EQ(utf8LowerCodepoint('Z'), static_cast<uint32_t>('z'));
  EXPECT_EQ(utf8LowerCodepoint(0x00C4), 0x00E4u);  // Ä
  EXPECT_EQ(utf8LowerCodepoint(0x00DE), 0x00FEu);  // Þ
  EXPECT_EQ(utf8LowerCodepoint(0x00D7), 0x00D7u);  // × is not a letter
  EXPECT_EQ(utf8LowerCodepoint(0x0141), 0x0142u);  // Ł, odd-aligned run
  EXPECT_EQ(utf8LowerCodepoint(0x0152), 0x0153u);  // Œ
  EXPECT_EQ(utf8LowerCodepoint(0x0178), 0x00FFu);  // Ÿ
  EXPECT_EQ(utf8LowerCodepoint(0x1E9E), 0x00DFu);  // ẞ
  // İ lowercases to a plain i; the dotless ı after it is a different letter.
  EXPECT_EQ(utf8LowerCodepoint(0x0130), static_cast<uint32_t>('i'));
  EXPECT_EQ(utf8LowerCodepoint(0x0218), 0x0219u);  // Ș
  EXPECT_EQ(utf8LowerCodepoint(0x01AF), 0x01B0u);  // Ư
  EXPECT_EQ(utf8LowerCodepoint(0x1EDE), 0x1EDFu);  // Ở
  // Already lowercase, or caseless: unchanged.
  EXPECT_EQ(utf8LowerCodepoint(0x00DF), 0x00DFu);  // ß
  EXPECT_EQ(utf8LowerCodepoint(0x0131), 0x0131u);  // ı
  EXPECT_EQ(utf8LowerCodepoint(0x1E96), 0x1E96u);  // ẖ, lowercase-only
}

TEST(Utf8Lower, Greek) {
  EXPECT_EQ(utf8LowerCodepoint(0x0391), 0x03B1u);  // Α
  EXPECT_EQ(utf8LowerCodepoint(0x03A1), 0x03C1u);  // Ρ
  EXPECT_EQ(utf8LowerCodepoint(0x03A3), 0x03C3u);  // Σ
  EXPECT_EQ(utf8LowerCodepoint(0x03AB), 0x03CBu);  // Ϋ
  EXPECT_EQ(utf8LowerCodepoint(0x0386), 0x03ACu);  // Ά
  EXPECT_EQ(utf8LowerCodepoint(0x0389), 0x03AEu);  // Ή
  EXPECT_EQ(utf8LowerCodepoint(0x038C), 0x03CCu);  // Ό
  EXPECT_EQ(utf8LowerCodepoint(0x038F), 0x03CEu);  // Ώ
  EXPECT_EQ(utf8LowerCodepoint(0x03A2), 0x03A2u);  // unassigned
  EXPECT_EQ(utf8LowerCodepoint(0x03C2), 0x03C2u);  // ς
}

TEST(Utf8Lower, Cyrillic) {
  EXPECT_EQ(utf8LowerCodepoint(0x0410), 0x0430u);  // А
  EXPECT_EQ(utf8LowerCodepoint(0x042F), 0x044Fu);  // Я
  EXPECT_EQ(utf8LowerCodepoint(0x0401), 0x0451u);  // Ё
  EXPECT_EQ(utf8LowerCodepoint(0x0404), 0x0454u);  // Є
  EXPECT_EQ(utf8LowerCodepoint(0x0406), 0x0456u);  // І
  EXPECT_EQ(utf8LowerCodepoint(0x0407), 0x0457u);  // Ї
  EXPECT_EQ(utf8LowerCodepoint(0x040F), 0x045Fu);  // Џ
  EXPECT_EQ(utf8LowerCodepoint(0x0490), 0x0491u);  // Ґ
  EXPECT_EQ(utf8LowerCodepoint(0x04D8), 0x04D9u);  // Ә
  EXPECT_EQ(utf8LowerCodepoint(0x04C0), 0x04CFu);  // Ӏ
  EXPECT_EQ(utf8LowerCodepoint(0x04C1), 0x04C2u);  // Ӂ, odd-aligned run
  EXPECT_EQ(utf8LowerCodepoint(0x052E), 0x052Fu);  // Ԯ
  EXPECT_EQ(utf8LowerCodepoint(0x0483), 0x0483u);  // combining titlo
  EXPECT_EQ(utf8LowerCodepoint(0x0451), 0x0451u);  // ё
}

TEST(Utf8Lower, OtherScriptsAreUnchanged) {
  EXPECT_EQ(utf8LowerCodepoint(0x05D0), 0x05D0u);  // א
  EXPECT_EQ(utf8LowerCodepoint(0x4E2D), 0x4E2Du);  // 中
  EXPECT_EQ(utf8LowerCodepoint(0xFF21), 0xFF21u);  // fullwidth Ａ, outside the covered blocks
  EXPECT_EQ(utf8LowerCodepoint(0x0000), 0x0000u);
}

TEST(Utf8Lower, WholeWord) {
  EXPECT_EQ(utf8ToLower("Über"), "über");
  EXPECT_EQ(utf8ToLower("STRAẞE"), "straße");  // three bytes become two
  EXPECT_EQ(utf8ToLower("İSTANBUL"), "istanbul");
  EXPECT_EQ(utf8ToLower("ПРИВЕТ"), "привет");
  EXPECT_EQ(utf8ToLower("already lower"), "already lower");
  EXPECT_EQ(utf8ToLower(""), "");
}

TEST(Utf8Lower, WordFinalSigma) {
  EXPECT_EQ(utf8ToLower("ΤΟΥΣ"), "τους");
  EXPECT_EQ(utf8ToLower("ΣΟΦΟΣ"), "σοφος");  // only the last one is final
  EXPECT_EQ(utf8ToLower("Σ"), "σ");          // a lone letter is not a word ending
}

TEST(Utf8Lower, UndecodableBytesPassThrough) {
  // A stray continuation byte and a truncated sequence: copied, not replaced by
  // a three-byte U+FFFD that would make the result longer than the input.
  const std::string broken =
      "A\x80"
      "B\xC3";
  EXPECT_EQ(utf8ToLower(broken),
            "a\x80"
            "b\xC3");
}

TEST(Utf8Upper, LatinBlocks) {
  EXPECT_EQ(utf8UpperCodepoint('a'), static_cast<uint32_t>('A'));
  EXPECT_EQ(utf8UpperCodepoint('z'), static_cast<uint32_t>('Z'));
  EXPECT_EQ(utf8UpperCodepoint(0x00E4), 0x00C4u);  // ä
  EXPECT_EQ(utf8UpperCodepoint(0x00FE), 0x00DEu);  // þ
  EXPECT_EQ(utf8UpperCodepoint(0x00F7), 0x00F7u);  // ÷ is not a letter
  EXPECT_EQ(utf8UpperCodepoint(0x00FF), 0x0178u);  // ÿ, whose capital sits above it
  EXPECT_EQ(utf8UpperCodepoint(0x0101), 0x0100u);  // ā
  EXPECT_EQ(utf8UpperCodepoint(0x0142), 0x0141u);  // ł, odd-aligned run
  EXPECT_EQ(utf8UpperCodepoint(0x017E), 0x017Du);  // ž, odd-aligned run
  EXPECT_EQ(utf8UpperCodepoint(0x0153), 0x0152u);  // œ
  EXPECT_EQ(utf8UpperCodepoint(0x0219), 0x0218u);  // ș
  EXPECT_EQ(utf8UpperCodepoint(0x01B0), 0x01AFu);  // ư
  EXPECT_EQ(utf8UpperCodepoint(0x1EDF), 0x1EDEu);  // ở
  // Lowercase-only letters whose capital is an ordinary one.
  EXPECT_EQ(utf8UpperCodepoint(0x0131), static_cast<uint32_t>('I'));  // ı
  EXPECT_EQ(utf8UpperCodepoint(0x017F), static_cast<uint32_t>('S'));  // ſ
  // Capitals, and letters with no single-codepoint capital: unchanged.
  EXPECT_EQ(utf8UpperCodepoint(0x0141), 0x0141u);  // Ł
  EXPECT_EQ(utf8UpperCodepoint(0x0178), 0x0178u);  // Ÿ
  EXPECT_EQ(utf8UpperCodepoint(0x00DF), 0x00DFu);  // ß (SS is two letters)
  EXPECT_EQ(utf8UpperCodepoint(0x0138), 0x0138u);  // ĸ has no capital
  EXPECT_EQ(utf8UpperCodepoint(0x0149), 0x0149u);  // ŉ (ʼN is two letters)
  EXPECT_EQ(utf8UpperCodepoint(0x1E96), 0x1E96u);  // ẖ, lowercase-only
}

TEST(Utf8Upper, Greek) {
  EXPECT_EQ(utf8UpperCodepoint(0x03B1), 0x0391u);  // α
  EXPECT_EQ(utf8UpperCodepoint(0x03C3), 0x03A3u);  // σ
  EXPECT_EQ(utf8UpperCodepoint(0x03C2), 0x03A3u);  // final ς
  EXPECT_EQ(utf8UpperCodepoint(0x03CB), 0x03ABu);  // ϋ
  EXPECT_EQ(utf8UpperCodepoint(0x03AC), 0x0386u);  // ά
  EXPECT_EQ(utf8UpperCodepoint(0x03AE), 0x0389u);  // ή
  EXPECT_EQ(utf8UpperCodepoint(0x03CC), 0x038Cu);  // ό
  EXPECT_EQ(utf8UpperCodepoint(0x03CE), 0x038Fu);  // ώ
  EXPECT_EQ(utf8UpperCodepoint(0x0390), 0x0390u);  // ΐ (its capital is three codepoints)
}

TEST(Utf8Upper, Cyrillic) {
  EXPECT_EQ(utf8UpperCodepoint(0x0430), 0x0410u);  // а
  EXPECT_EQ(utf8UpperCodepoint(0x044F), 0x042Fu);  // я
  EXPECT_EQ(utf8UpperCodepoint(0x0451), 0x0401u);  // ё
  EXPECT_EQ(utf8UpperCodepoint(0x045F), 0x040Fu);  // џ
  EXPECT_EQ(utf8UpperCodepoint(0x0491), 0x0490u);  // ґ
  EXPECT_EQ(utf8UpperCodepoint(0x04CF), 0x04C0u);  // ӏ
  EXPECT_EQ(utf8UpperCodepoint(0x04C2), 0x04C1u);  // ӂ, odd-aligned run
  EXPECT_EQ(utf8UpperCodepoint(0x052F), 0x052Eu);  // ԯ
  EXPECT_EQ(utf8UpperCodepoint(0x0416), 0x0416u);  // Ж
}

TEST(Utf8Upper, OtherScriptsAreUnchanged) {
  EXPECT_EQ(utf8UpperCodepoint(0x05D0), 0x05D0u);  // א
  EXPECT_EQ(utf8UpperCodepoint(0x4E2D), 0x4E2Du);  // 中
  EXPECT_EQ(utf8UpperCodepoint(0xFB01), 0xFB01u);  // ﬁ ligature
  EXPECT_EQ(utf8UpperCodepoint(0x0000), 0x0000u);
}

// The two directions are one mapping: every capital utf8LowerCodepoint() lowercases comes back
// from utf8UpperCodepoint(), and every letter utf8UpperCodepoint() capitalises lowercases back,
// across the whole BMP. A rule added to one side alone fails here.
TEST(Utf8Upper, InvertsUtf8LowerCodepoint) {
  for (uint32_t cp = 0; cp <= 0xFFFF; cp++) {
    const uint32_t lower = utf8LowerCodepoint(cp);
    // İ and ẞ lowercase to i and ß, whose own capitals are I and (no single letter) ß.
    if (lower != cp && cp != 0x0130 && cp != 0x1E9E) {
      EXPECT_EQ(utf8UpperCodepoint(lower), cp) << "U+" << std::hex << cp;
    }
    const uint32_t upper = utf8UpperCodepoint(cp);
    // ı, ſ and ς share their capital with i, s and σ.
    if (upper != cp && cp != 0x0131 && cp != 0x017F && cp != 0x03C2) {
      EXPECT_EQ(utf8LowerCodepoint(upper), cp) << "U+" << std::hex << cp;
    }
  }
}

// As with utf8ToLower(), a capitalised copy of a string fits in the space the original took.
TEST(Utf8Upper, NeverLengthensTheEncoding) {
  const auto encodedLength = [](const uint32_t cp) { return cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4; };
  for (uint32_t cp = 0; cp <= 0xFFFF; cp++) {
    EXPECT_LE(encodedLength(utf8UpperCodepoint(cp)), encodedLength(cp)) << "U+" << std::hex << cp;
  }
}

}  // namespace
