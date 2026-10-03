#include <OtShaper.h>
#include <OtUnicodeData.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

// lib/OtShaper on the .cpfont layout fonts in test/complex_shaper/data. Its
// output is checked against HarfBuzz glyph for glyph by ComplexShaperTest
// (and at scale by compare_harfbuzz.py); these tests cover the parts that
// test cannot see: the precomputed lookup filters and plans, and hostile fonts.

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

void expectSamePlan(const ot::Plan& built, const ot::Plan& loaded, const std::string& where) {
  SCOPED_TRACE(where);
  EXPECT_EQ(built.script, loaded.script);
  EXPECT_EQ(built.shaper, loaded.shaper);
  EXPECT_EQ(built.chosenScript, loaded.chosenScript);
  EXPECT_EQ(built.globalMask, loaded.globalMask);
  for (int t = 0; t < 2; t++) {
    ASSERT_EQ(built.lookups[t].size(), loaded.lookups[t].size());
    for (size_t i = 0; i < built.lookups[t].size(); i++) {
      EXPECT_EQ(built.lookups[t][i].index, loaded.lookups[t][i].index);
      EXPECT_EQ(built.lookups[t][i].flags, loaded.lookups[t][i].flags);
      EXPECT_EQ(built.lookups[t][i].mask, loaded.lookups[t][i].mask);
    }
    ASSERT_EQ(built.stages[t].size(), loaded.stages[t].size());
    for (size_t i = 0; i < built.stages[t].size(); i++) {
      EXPECT_EQ(built.stages[t][i].lastLookup, loaded.stages[t][i].lastLookup);
      EXPECT_EQ(built.stages[t][i].pause, loaded.stages[t][i].pause);
    }
  }
  EXPECT_EQ(built.applyGpos, loaded.applyGpos);
  EXPECT_EQ(built.fallbackGlyphClasses, loaded.fallbackGlyphClasses);
  EXPECT_EQ(built.zeroMarks, loaded.zeroMarks);
  EXPECT_EQ(built.indicConfig, loaded.indicConfig);
  EXPECT_EQ(built.isOldSpec, loaded.isOldSpec);
  EXPECT_EQ(0, std::memcmp(built.indicMasks, loaded.indicMasks, sizeof(built.indicMasks)));
  EXPECT_EQ(0, std::memcmp(built.wouldStage, loaded.wouldStage, sizeof(built.wouldStage)));
  EXPECT_EQ(built.useRphfMask, loaded.useRphfMask);
  EXPECT_EQ(
      0, std::memcmp(built.useTopographicalMasks, loaded.useTopographicalMasks, sizeof(built.useTopographicalMasks)));
}

// The converter compiles the plans in Python (shaping_blob.py); loading one
// must give exactly the plan the shaper builds from the font, in every
// language the firmware maps and for mixed requests.
TEST(OtPlanCompiled, LoadedPlansMatchBuiltPlans) {
  std::vector<std::vector<uint32_t>> requests = {{0}};
  for (const auto& language : ot::ucd::LANGUAGES) requests.emplace_back(language.tags, language.tags + 3);
  requests.push_back({ot::tag("MAR "), ot::tag("NEP "), 0});
  requests.push_back({ot::tag("XXXX"), ot::tag("SAN "), ot::tag("MAR ")});
  requests.push_back({ot::tag("dflt"), 0, 0});
  for (const Fixture& fx : fixtures()) {
    const std::vector<uint8_t> font = readFixture(fx.file);
    ot::Face face;
    ASSERT_TRUE(face.init(tablesOf(font)));
    ASSERT_FALSE(face.compiledPlans().empty()) << fx.file;
    for (const std::vector<uint32_t>& tags : requests) {
      const std::string where = std::string(fx.file) + " language " + std::to_string(tags[0]);
      ot::Plan built, loaded;
      ASSERT_TRUE(built.build(face, fx.script, tags.data())) << where;
      ASSERT_TRUE(loaded.load(face, fx.script, tags.data())) << where;
      expectSamePlan(built, loaded, where);
      for (const char* word : fx.words) EXPECT_EQ(shapeWord(face, built, word), shapeWord(face, loaded, word));
    }
  }
}

TEST(OtPlanCompiled, FontsWithoutPlansOrForOtherScriptsLoadNothing) {
  const std::vector<uint8_t> font = readFixture("NotoSansTamil-Regular.layout");
  ot::FaceTables tables = tablesOf(font);
  ot::Face face;
  ASSERT_TRUE(face.init(tables));
  ot::Plan plan;
  EXPECT_FALSE(plan.load(face, ot::Script::Bengali, ot::languageTagsFor("")));
  tables[ot::FaceTables::CPPL] = ot::Table();
  ASSERT_TRUE(face.init(tables));
  EXPECT_FALSE(plan.load(face, ot::Script::Tamil, ot::languageTagsFor("")));
}

// A compiled plan body in a CPpl table, for editing: the shaper byte and, per
// table, the 4-byte stage records (lastLookup u16, pause u8, reserved u8).
struct CompiledPlanEdit {
  uint8_t* shaper;
  uint8_t* stages[2];
  uint16_t stageCount[2];
};

// A copy of `compiled` with `edit` applied to every plan body.
std::vector<uint8_t> editCompiledPlans(const ot::Table& compiled, void (*edit)(const CompiledPlanEdit&)) {
  std::vector<uint8_t> bytes(compiled.data(), compiled.data() + compiled.length());
  const auto u16 = [&](const uint32_t at) { return static_cast<uint16_t>(bytes[at] << 8 | bytes[at + 1]); };
  const auto u32 = [&](const uint32_t at) { return static_cast<uint32_t>(u16(at)) << 16 | u16(at + 2); };
  constexpr uint32_t TABLES = 12 + 4 * ot::MF_COUNT + 8;  // cppl::TABLES in OtPlan.cpp
  for (uint16_t i = 0; i < u16(2); i++) {
    const uint32_t body = u32(4 + 14 * i + 10);
    CompiledPlanEdit plan{&bytes[body + 4], {}, {}};
    uint32_t at = body + TABLES;
    for (int t = 0; t < 2; t++) {
      plan.stageCount[t] = u16(at);
      plan.stages[t] = &bytes[at + 4];
      at += 4 + 4 * u16(at) + 8 * u16(at + 2);
    }
    edit(plan);
  }
  return bytes;
}

// A malformed .cpfont must not load a plan whose pauses would run a shaper's
// code without its data (Plan::indicConfig, INDIC_CONFIGS).
TEST(OtPlanCompiled, RejectsPlansInconsistentWithTheirShaper) {
  const auto loads = [](const char* file, const ot::Script script, void (*edit)(const CompiledPlanEdit&)) {
    const std::vector<uint8_t> font = readFixture(file);
    ot::FaceTables tables = tablesOf(font);
    const std::vector<uint8_t> plans = editCompiledPlans(tables[ot::FaceTables::CPPL], edit);
    tables[ot::FaceTables::CPPL] = ot::Table(plans.data(), static_cast<uint32_t>(plans.size()));
    ot::Face face;
    EXPECT_TRUE(face.init(tables)) << file;
    ot::Plan plan;
    const bool loaded = plan.load(face, script, ot::languageTagsFor(""));
    shapeWord(face, plan, "ශ්‍රී");  // a plan that failed to load is still safe to shape with
    return loaded;
  };
  const char* tamil = "NotoSansTamil-Regular.layout";
  const char* sinhala = "NotoSansSinhala-Regular.layout";
  ASSERT_TRUE(loads(tamil, ot::Script::Tamil, [](const CompiledPlanEdit&) {}));
  ASSERT_TRUE(loads(sinhala, ot::Script::Sinhala, [](const CompiledPlanEdit&) {}));

  // Indic pauses in a plan marked Default.
  EXPECT_FALSE(loads(tamil, ot::Script::Tamil, [](const CompiledPlanEdit& plan) {
    *plan.shaper = static_cast<uint8_t>(ot::ShaperKind::Default);
  }));
  // USE pauses in a plan marked Indic.
  EXPECT_FALSE(loads(tamil, ot::Script::Tamil, [](const CompiledPlanEdit& plan) {
    for (uint16_t i = 0; i < plan.stageCount[0]; i++) {
      if (plan.stages[0][4 * i + 2] != 0) plan.stages[0][4 * i + 2] = static_cast<uint8_t>(ot::Pause::UseReorder);
    }
  }));
  // A pause after a GPOS stage.
  EXPECT_FALSE(loads(tamil, ot::Script::Tamil, [](const CompiledPlanEdit& plan) {
    ASSERT_GT(plan.stageCount[1], 0);
    plan.stages[1][2] = static_cast<uint8_t>(ot::Pause::IndicSetupSyllables);
  }));
  // The Indic shaper for Sinhala, which has no Indic config, even without pauses.
  EXPECT_FALSE(loads(sinhala, ot::Script::Sinhala, [](const CompiledPlanEdit& plan) {
    *plan.shaper = static_cast<uint8_t>(ot::ShaperKind::Indic);
    for (int t = 0; t < 2; t++) {
      for (uint16_t i = 0; i < plan.stageCount[t]; i++) plan.stages[t][4 * i + 2] = 0;
    }
  }));
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
      ot::Plan built, loaded;
      built.build(face, fx.script, ot::languageTagsFor(""));
      for (const char* word : fx.words) shapeWord(face, built, word);
      if (loaded.load(face, fx.script, ot::languageTagsFor(""))) {
        for (const char* word : fx.words) shapeWord(face, loaded, word);
      }
    }
  }
  SUCCEED();
}

// Big-endian bytes of a hand-built table.
struct TableBytes {
  std::vector<uint8_t> bytes;
  TableBytes& u16(const unsigned v) {
    bytes.push_back(static_cast<uint8_t>(v >> 8));
    bytes.push_back(static_cast<uint8_t>(v));
    return *this;
  }
  TableBytes& u32(const uint32_t v) { return u16(v >> 16).u16(v & 0xFFFF); }
};

// A fixture font with its GSUB replaced by `gsub`.
bool initWithGsub(ot::Face& face, const std::vector<uint8_t>& font, const std::vector<uint8_t>& gsub) {
  ot::FaceTables tables = tablesOf(font, false);
  tables[ot::FaceTables::GSUB_TABLE] = ot::Table(gsub.data(), static_cast<uint32_t>(gsub.size()));
  return face.init(tables);
}

// A GSUB with one Extension lookup of two subtables, extending `first` and
// `second` (1: SingleSubst, `singleGlyph` + 1; 8: ReverseChainSingleSubst,
// `reverseGlyph` to itself).
std::vector<uint8_t> extensionGsub(const uint16_t first, const uint16_t second, const uint16_t singleGlyph,
                                   const uint16_t reverseGlyph) {
  constexpr uint32_t EXTENSIONS = 24, SINGLE = 40, REVERSE = 52;
  TableBytes t;
  t.u16(1).u16(0).u16(0).u16(0).u16(10);  // header: v1.0, no scripts or features, lookups at 10
  t.u16(1).u16(4);                        // 10: LookupList, one lookup at 14
  t.u16(7).u16(0).u16(2).u16(EXTENSIONS - 14).u16(EXTENSIONS + 8 - 14);  // 14: Extension lookup
  for (uint32_t i = 0; i < 2; i++) {
    const uint16_t type = i == 0 ? first : second;
    t.u16(1).u16(type).u32((type == 1 ? SINGLE : REVERSE) - (EXTENSIONS + 8 * i));  // 24, 32: ExtensionFormat1
  }
  t.u16(1).u16(6).u16(1);                                   // 40: SingleSubstFormat1, delta 1
  t.u16(1).u16(1).u16(singleGlyph);                         // 46: its coverage
  t.u16(1).u16(12).u16(0).u16(0).u16(1).u16(reverseGlyph);  // 52: ReverseChainSingleSubstFormat1
  t.u16(1).u16(1).u16(reverseGlyph);                        // 64: its coverage
  return t.bytes;
}

// HarfBuzz drops an Extension lookup whose subtables extend different types.
// Applied, such a lookup could leave its glyph unmoved and repeat forever.
TEST(OtShaperRobustness, DropsExtensionLookupsThatMixSubtableTypes) {
  const std::vector<uint8_t> font = readFixture("NotoSansTamil-Regular.layout");
  const auto apply = [&](const uint16_t first, const uint16_t second, const uint16_t singleGlyph,
                         const uint16_t reverseGlyph) {
    const std::vector<uint8_t> gsub = extensionGsub(first, second, singleGlyph, reverseGlyph);
    ot::Face face;
    EXPECT_TRUE(initWithGsub(face, font, gsub));
    ot::Buffer buffer;
    EXPECT_TRUE(buffer.prepare(8));
    for (int i = 0; i < 3; i++) {
      ot::GlyphInfo g{};
      g.codepoint = 5;
      g.mask = 1;
      buffer.info.push_back(g);
    }
    ot::Scale scale;
    scale.set(2133, 33, face.upem());
    ot::LookupSettings settings;
    settings.mask = 1;
    ot::applyLookup(face, scale, buffer, ot::GSUB, 0, settings);
    std::vector<uint32_t> glyphs;
    for (const ot::GlyphInfo& g : buffer.info) glyphs.push_back(g.codepoint);
    return glyphs;
  };
  EXPECT_EQ(apply(1, 1, 5, 9), (std::vector<uint32_t>{6, 6, 6}));
  // A forward lookup whose reverse chaining subtable matches, and a reverse
  // lookup whose single substitution does.
  EXPECT_EQ(apply(1, 8, 9, 5), (std::vector<uint32_t>{5, 5, 5}));
  EXPECT_EQ(apply(8, 1, 5, 9), (std::vector<uint32_t>{5, 5, 5}));
}

// A FeatureVariations condition tree whose AND nodes all share their
// children asks for 255^8 evaluations; planning must still finish.
TEST(OtShaperRobustness, BoundsFeatureVariationConditionWork) {
  constexpr uint32_t CONDITIONS = 36, AND_BYTES = 3 + 3 * 255, LEVELS = 8;
  TableBytes t;
  t.u16(1).u16(1).u16(0).u16(0).u16(0).u32(14);        // header v1.1: no scripts, features or lookups
  t.u16(1).u16(0).u32(1).u32(30 - 14).u32(0);          // 14: FeatureVariations, one record
  t.u16(1).u32(CONDITIONS - 30);                       // 30: ConditionSet, one condition
  for (uint32_t level = 0; level < LEVELS; level++) {  // 36: ConditionAnd nodes
    t.bytes.push_back(0);
    t.bytes.push_back(3);
    t.bytes.push_back(255);
    for (int i = 0; i < 255; i++) {
      t.bytes.insert(t.bytes.end(), {0, static_cast<uint8_t>(AND_BYTES >> 8), static_cast<uint8_t>(AND_BYTES)});
    }
  }
  t.u16(1).u16(0).u16(0xC000).u16(0x4000);  // axis range -1..1: holds at the default instance
  ASSERT_EQ(t.bytes.size(), CONDITIONS + LEVELS * AND_BYTES + 8);

  const std::vector<uint8_t> font = readFixture("NotoSansTamil-Regular.layout");
  ot::Face face;
  ASSERT_TRUE(initWithGsub(face, font, t.bytes));
  ot::Plan plan;
  plan.build(face, ot::Script::Tamil, ot::languageTagsFor(""));
  shapeWord(face, plan, "தமிழ்");
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
