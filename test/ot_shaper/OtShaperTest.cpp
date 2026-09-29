#include <OtShaper.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

// lib/OtShaper on the .cpfont layout fonts in test/complex_shaper/data. Its
// output is checked against HarfBuzz glyph for glyph by ComplexShaperTest
// (and at scale by compare_harfbuzz.py); these tests cover the parts that
// test cannot see: the precomputed lookup filters and hostile fonts.

namespace {

std::vector<uint8_t> readFixture(const char* name) {
  std::vector<uint8_t> bytes;
  if (FILE* f = std::fopen((std::string(SHAPING_FIXTURE_DIR) + "/" + name).c_str(), "rb")) {
    uint8_t buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
    std::fclose(f);
  }
  return bytes;
}

// Table views of an sfnt in memory; `withFilters` false drops CPac.
ot::FaceTables tablesOf(const std::vector<uint8_t>& font, const bool withFilters = true) {
  ot::FaceTables t;
  ot::tablesFromSfnt(font.data(), static_cast<uint32_t>(font.size()), &t);
  if (!withFilters) t[ot::FaceTables::CPAC] = ot::Table();
  return t;
}

std::vector<uint32_t> utf32(const char* utf8) {
  std::vector<uint32_t> out;
  const auto* p = reinterpret_cast<const unsigned char*>(utf8);
  while (*p) {
    uint32_t cp = *p;
    int n = cp < 0x80 ? 1 : (cp >> 5) == 6 ? 2 : (cp >> 4) == 14 ? 3 : 4;
    cp = n == 1 ? cp : n == 2 ? cp & 0x1F : n == 3 ? cp & 0x0F : cp & 0x07;
    for (int k = 1; k < n && p[k]; k++) cp = (cp << 6) | (p[k] & 0x3F);
    out.push_back(cp);
    p += n;
  }
  return out;
}

struct Fixture {
  const char* file;
  ot::Script script;
  std::vector<const char*> words;
};

const std::vector<Fixture>& fixtures() {
  static const std::vector<Fixture> all = {
      {"NotoSansBengali-Regular.layout",
       ot::Script::Bengali,
       {"শ্রীঈশ্বরচন্দ্র", "কর্ত্তৃক", "সর্ব্বোৎকৃষ্ট", "বিদ্যাসাগর", "বাক্‌শক্তি", "হৃদয়", "ক্ষি", "র্যা"}},
      {"NotoSansDevanagari-Regular.layout",
       ot::Script::Devanagari,
       {"हिन्दी", "प्रार्थना", "र्‍य", "संस्कृतम्", "क़िला", "द्ध्र्य"}},
      {"NotoSansTamil-Regular.layout", ot::Script::Tamil, {"பொன்னியின்", "ஸ்ரீ", "க்ஷ"}},
      {"NotoSansSinhala-Regular.layout", ot::Script::Sinhala, {"ශ්‍රී", "ක්‍ෂ", "කෞ", "සිංහල"}},
  };
  return all;
}

// Glyph IDs and positions of a shaped word, flattened for comparison.
std::vector<int32_t> shapeWord(const ot::Face& face, const ot::Plan& plan, const char* word) {
  ot::Scale scale;
  scale.set(2133, 33, face.upem());
  ot::Buffer buffer;
  const std::vector<uint32_t> cps = utf32(word);
  std::vector<int32_t> out;
  if (!ot::shape(face, scale, plan, cps.data(), static_cast<unsigned>(cps.size()), buffer)) return out;
  for (unsigned i = 0; i < buffer.len(); i++) {
    out.insert(out.end(), {static_cast<int32_t>(buffer.info[i].codepoint), buffer.pos[i].xAdvance,
                           buffer.pos[i].xOffset, buffer.pos[i].yOffset});
  }
  return out;
}

TEST(OtShaperFilters, ConverterFiltersMatchWhatTheShaperComputes) {
  for (const Fixture& fx : fixtures()) {
    const std::vector<uint8_t> font = readFixture(fx.file);
    ASSERT_FALSE(font.empty()) << fx.file;
    ot::Face face;
    ASSERT_TRUE(face.init(tablesOf(font)));
    for (int t = 0; t < 2; t++) {
      const ot::FilterRecords* r = face.filters(t);
      ASSERT_NE(r, nullptr) << fx.file << " table " << t;
      uint32_t subtable = 0;
      for (uint32_t i = 0; i < face.lookupCount(t); i++) {
        const ot::Digest computed = ot::lookupDigest(face, t, i);
        const ot::Digest stored = ot::readDigest(r->lookupDigests + ot::DIGEST_BYTES * i);
        EXPECT_EQ(0, std::memcmp(computed.masks, stored.masks, sizeof(computed.masks))) << fx.file << " lookup " << i;
        std::vector<uint8_t> subtables;
        ot::appendSubtableDigests(face, t, i, subtables);
        EXPECT_EQ(r->firstSubtable(i), subtable);
        EXPECT_EQ(0, std::memcmp(subtables.data(), r->subtableDigests + ot::DIGEST_BYTES * subtable, subtables.size()));
        subtable += static_cast<uint32_t>(subtables.size() / ot::DIGEST_BYTES);
      }
    }
  }
}

// Allows the plan's lookup lists (one reservation each for GSUB and GPOS)
// and refuses the lookup filters Plan::build() computes after them.
int gPlanReservationsLeft = 0;
bool refuseFilterHeap(size_t) { return gPlanReservationsLeft-- > 0; }

TEST(OtShaperFilters, ShapingIsTheSameWithStoredComputedOrNoFilters) {
  for (const Fixture& fx : fixtures()) {
    const std::vector<uint8_t> font = readFixture(fx.file);
    ot::Face stored, computed;
    ASSERT_TRUE(stored.init(tablesOf(font)));
    ASSERT_TRUE(computed.init(tablesOf(font, false)));
    ot::Plan withStored, withComputed, without;
    ASSERT_TRUE(withStored.build(stored, fx.script, ot::languageTagsFor("")));
    ASSERT_TRUE(withComputed.build(computed, fx.script, ot::languageTagsFor("")));
    gPlanReservationsLeft = 2;
    ot::setHeapCheck(refuseFilterHeap);
    const bool builtWithout = without.build(computed, fx.script, ot::languageTagsFor(""));
    ot::setHeapCheck(nullptr);
    ASSERT_TRUE(builtWithout);
    for (const char* word : fx.words) {
      const std::vector<int32_t> expected = shapeWord(stored, withStored, word);
      ASSERT_FALSE(expected.empty()) << word;
      EXPECT_EQ(expected, shapeWord(computed, withComputed, word)) << word;
      EXPECT_EQ(expected, shapeWord(computed, without, word)) << word;
    }
  }
}

TEST(OtShaperFilters, MismatchedFiltersAreIgnored) {
  const std::vector<uint8_t> font = readFixture("NotoSansBengali-Regular.layout");
  ot::FaceTables tables = tablesOf(font);
  const ot::Table& original = tables[ot::FaceTables::CPAC];
  std::vector<uint8_t> cpac(original.data(), original.data() + original.length());
  ASSERT_GT(cpac.size(), 5u);
  cpac[5] ^= 1;  // GSUB lookup count no longer matches the font
  tables[ot::FaceTables::CPAC] = ot::Table(cpac.data(), static_cast<uint32_t>(cpac.size()));
  ot::Face face;
  ASSERT_TRUE(face.init(tables));
  EXPECT_EQ(face.filters(ot::GSUB), nullptr);
}

// Truncated and corrupted fonts must shape to something or fail cleanly,
// never read out of bounds (run under -fsanitize=address to check).
TEST(OtShaperRobustness, SurvivesTruncatedAndCorruptedFonts) {
  std::mt19937 rng(1757);
  for (const Fixture& fx : fixtures()) {
    const std::vector<uint8_t> original = readFixture(fx.file);
    for (int round = 0; round < 40; round++) {
      std::vector<uint8_t> font = original;
      if (round % 2 == 0) {
        font.resize(std::uniform_int_distribution<size_t>(0, font.size())(rng));
      } else {
        for (int k = 0; k < 64; k++)
          font[std::uniform_int_distribution<size_t>(0, font.size() - 1)(rng)] = rng() & 0xFF;
      }
      ot::Face face;
      if (!face.init(tablesOf(font))) continue;
      ot::Plan plan;
      plan.build(face, fx.script, ot::languageTagsFor(""));
      for (const char* word : fx.words) shapeWord(face, plan, word);
    }
  }
  SUCCEED();
}

TEST(OtShaperLanguage, MapsBcp47TagsToLanguageSystems) {
  EXPECT_EQ(ot::languageTagsFor("mr")[0], ot::tag("MAR "));
  EXPECT_EQ(ot::languageTagsFor("ne-NP")[0], ot::tag("NEP "));
  EXPECT_EQ(ot::languageTagsFor("BN_bd")[0], ot::tag("BEN "));
  EXPECT_EQ(ot::languageTagsFor("xx")[0], 0u);
  EXPECT_EQ(ot::languageTagsFor("")[0], 0u);
  EXPECT_EQ(ot::languageTagsFor(nullptr)[0], 0u);
}

}  // namespace
