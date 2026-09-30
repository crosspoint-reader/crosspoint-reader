#include <Arduino.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "ComplexShaper.h"
#include "ExpectedShaping.h"
#include "OtFace.h"
#include "ShapingTokens.h"
#include "Utf8.h"

namespace {

std::vector<uint8_t> readLayout(const char* file) {
  std::vector<uint8_t> bytes;
  if (FILE* f = std::fopen((std::string(SHAPING_FIXTURE_DIR) + "/" + file).c_str(), "rb")) {
    uint8_t buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
    std::fclose(f);
  }
  return bytes;
}

// The Bengali layout blob, which most tests shape with.
const std::vector<uint8_t>& fixture() {
  static const std::vector<uint8_t> bytes = readLayout("NotoSansBengali-Regular.layout");
  return bytes;
}

int gLoads = 0;

// ctx: the layout blob to load (const std::vector<uint8_t>*), or nullptr for fixture().
bool loadFixture(void* ctx, ComplexShaper::Blob* out) {
  gLoads++;
  const auto& bytes = ctx != nullptr ? *static_cast<const std::vector<uint8_t>*>(ctx) : fixture();
  auto* copy = static_cast<uint8_t*>(ComplexShaper::allocate(bytes.size()));
  if (copy == nullptr) return false;
  std::memcpy(copy, bytes.data(), bytes.size());
  *out = ComplexShaper::Blob{copy, static_cast<uint32_t>(bytes.size()), nullptr};
  return true;
}

constexpr uint32_t kFixtureKey = 0x1234567u;

struct DecodedGlyph {
  uint32_t gid;
  int advance12_4;
  int dx;
  int dy;
  bool operator==(const DecodedGlyph&) const = default;
};

// Splits a token stream into glyphs; non-token codepoints land in `plain`.
std::vector<DecodedGlyph> decode(const std::string& tokens, std::string* plain = nullptr) {
  std::vector<DecodedGlyph> glyphs;
  int advance = -1, dx = 0, dy = 0;
  const auto* p = reinterpret_cast<const unsigned char*>(tokens.c_str());
  while (const uint32_t cp = utf8NextCodepoint(&p)) {
    if (shaping::isAdvanceToken(cp)) {
      advance = shaping::advanceTokenValue(cp);
    } else if (shaping::isOffsetToken(cp)) {
      dx = shaping::offsetTokenDx(cp);
      dy = shaping::offsetTokenDy(cp);
    } else if (shaping::isGlyphToken(cp)) {
      glyphs.push_back({shaping::glyphTokenId(cp), advance, dx, dy});
      advance = -1;
      dx = dy = 0;
    } else if (plain != nullptr) {
      utf8AppendCodepoint(cp, *plain);
    }
  }
  return glyphs;
}

std::vector<uint32_t> glyphIds(const std::string& tokens) {
  std::vector<uint32_t> gids;
  for (const auto& g : decode(tokens)) gids.push_back(g.gid);
  return gids;
}

std::vector<uint32_t> glyphIds(const ExpectedShaping& entry) {
  std::vector<uint32_t> gids;
  for (uint8_t i = 0; i < entry.count; i++) gids.push_back(entry.glyphs[i].gid);
  return gids;
}

class ComplexShaperTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_FALSE(fixture().empty()) << "missing layout fixtures in " << SHAPING_FIXTURE_DIR;
    ComplexShaper::setMemoryBudget(4 << 20);
    shaper.setBlobSource(loadFixture, nullptr, kFixtureKey);
    shaper.setScale(kFixturePpem26_6);
    gLoads = 0;
  }
  void TearDown() override {
    ComplexShaper::setDocumentLanguage("");
    ComplexShaper::releaseAll();
  }

  ComplexShaper shaper;
};

}  // namespace

TEST(ShapingTokens, RoundTripAndStayDisjoint) {
  for (const uint32_t gid : {0u, 1u, 384u, shaping::GLYPH_TOKEN_MAX_GID}) {
    EXPECT_TRUE(shaping::isGlyphToken(shaping::glyphToken(gid)));
    EXPECT_EQ(shaping::glyphTokenId(shaping::glyphToken(gid)), gid);
    EXPECT_FALSE(shaping::isPositionToken(shaping::glyphToken(gid)));
  }
  EXPECT_EQ(shaping::advanceTokenValue(shaping::advanceToken(0)), 0);
  EXPECT_EQ(shaping::advanceTokenValue(shaping::advanceToken(430)), 430);
  EXPECT_EQ(shaping::advanceTokenValue(shaping::advanceToken(-5)), 0);  // clamped
  EXPECT_EQ(shaping::advanceTokenValue(shaping::advanceToken(1 << 20)), shaping::ADVANCE_TOKEN_MAX);
  for (const int dx : {-128, -9, 0, 7, 127}) {
    for (const int dy : {-128, -1, 0, 3, 127}) {
      const uint32_t t = shaping::offsetToken(dx, dy);
      EXPECT_TRUE(shaping::isOffsetToken(t));
      EXPECT_FALSE(shaping::isGlyphToken(t));
      EXPECT_EQ(shaping::offsetTokenDx(t), dx);
      EXPECT_EQ(shaping::offsetTokenDy(t), dy);
    }
  }
  EXPECT_EQ(shaping::offsetTokenDx(shaping::offsetToken(300, 0)), 127);  // clamped
  // Every token survives the renderer's UTF-8 round trip.
  for (const uint32_t cp : {shaping::glyphToken(0), shaping::advanceToken(shaping::ADVANCE_TOKEN_MAX),
                            shaping::offsetToken(-1, -1), shaping::offsetToken(127, 127)}) {
    std::string utf8;
    utf8AppendCodepoint(cp, utf8);
    const auto* p = reinterpret_cast<const unsigned char*>(utf8.c_str());
    EXPECT_EQ(utf8NextCodepoint(&p), cp);
  }
}

TEST_F(ComplexShaperTest, LeavesTextWithoutIndicAlone) {
  std::string out;
  EXPECT_FALSE(shaper.shape("hello world", out));
  EXPECT_FALSE(shaper.shape("\xE0\xA5\xA4", out));  // a lone danda
  EXPECT_EQ(gLoads, 0) << "no layout tables should load for text that needs no shaping";
}

TEST_F(ComplexShaperTest, MatchesHarfBuzzReferenceShapingInEveryScript) {
  for (size_t f = 0; f < sizeof(kShapingFixtures) / sizeof(kShapingFixtures[0]); f++) {
    const ShapingFixture& fixtureScript = kShapingFixtures[f];
    const std::vector<uint8_t> layout = readLayout(fixtureScript.layoutFile);
    ASSERT_FALSE(layout.empty()) << "missing " << fixtureScript.layoutFile;
    ComplexShaper scriptShaper;
    scriptShaper.setBlobSource(loadFixture, const_cast<std::vector<uint8_t>*>(&layout), kFixtureKey + 1 + f);
    scriptShaper.setScale(kFixturePpem26_6);
    for (size_t w = 0; w < fixtureScript.count; w++) {
      const ExpectedShaping& expected = fixtureScript.words[w];
      ComplexShaper::setDocumentLanguage(expected.language);
      std::string out;
      ASSERT_TRUE(scriptShaper.shape(expected.utf8, out)) << fixtureScript.script << ": " << expected.utf8;
      std::vector<DecodedGlyph> want;
      for (uint8_t i = 0; i < expected.count; i++) {
        const ExpectedGlyph& g = expected.glyphs[i];
        want.push_back({g.gid, g.advance12_4, g.dx, g.dy});
      }
      EXPECT_EQ(decode(out), want) << fixtureScript.script << ": " << expected.utf8 << " (" << expected.language << ")";
    }
    ComplexShaper::setDocumentLanguage("");
  }
}

TEST_F(ComplexShaperTest, LeavesScriptsTheFontDoesNotCoverAsText) {
  const std::string devanagari = "\xE0\xA4\x95\xE0\xA4\xBF";  // कि
  const std::string bengali = "\xE0\xA6\x95\xE0\xA6\xBF";     // কি
  std::string out;
  EXPECT_FALSE(shaper.shape(devanagari.c_str(), out)) << "the Bengali tables cannot shape Devanagari";
  ASSERT_TRUE(shaper.shape((devanagari + " " + bengali).c_str(), out));
  std::string plain;
  EXPECT_EQ(decode(out, &plain).size(), 2u);  // i-matra then ka, from the Bengali run only
  EXPECT_EQ(plain, devanagari + " ");
  EXPECT_EQ(out.rfind(devanagari + " ", 0), 0u);
}

TEST_F(ComplexShaperTest, SplitsRunsWhereTheScriptChanges) {
  // কি immediately followed by कि: two runs, the second left as text.
  const std::string mixed = "\xE0\xA6\x95\xE0\xA6\xBF\xE0\xA4\x95\xE0\xA4\xBF";
  std::string out;
  ASSERT_TRUE(shaper.shape(mixed.c_str(), out));
  std::string plain;
  EXPECT_EQ(decode(out, &plain).size(), 2u);
  EXPECT_EQ(plain, "\xE0\xA4\x95\xE0\xA4\xBF");
}

TEST_F(ComplexShaperTest, ShapesInTheDocumentLanguageAndReshapesWhenItChanges) {
  const std::vector<uint8_t> layout = readLayout("NotoSansDevanagari-Regular.layout");
  ASSERT_FALSE(layout.empty());
  ComplexShaper devanagari;
  devanagari.setBlobSource(loadFixture, const_cast<std::vector<uint8_t>*>(&layout), kFixtureKey + 100);
  devanagari.setScale(kFixturePpem26_6);
  const char* word = "\xE0\xA4\xB2";  // ल: Marathi fonts draw it differently
  std::string hindi, marathi, again;
  ASSERT_TRUE(devanagari.shape(word, hindi));
  ComplexShaper::setDocumentLanguage("mr-IN");
  ASSERT_TRUE(devanagari.shape(word, marathi));
  ComplexShaper::setDocumentLanguage("");
  ASSERT_TRUE(devanagari.shape(word, again));
  EXPECT_NE(decode(hindi), decode(marathi)) << "a cached Hindi run must not serve Marathi text";
  EXPECT_EQ(hindi, again);
}

TEST_F(ComplexShaperTest, CopiesTextAroundRunsVerbatim) {
  std::string out;
  ASSERT_TRUE(shaper.shape("a \xE0\xA6\x95\xE0\xA6\xBF, b", out));  // "a কি, b"
  std::string plain;
  const auto glyphs = decode(out, &plain);
  EXPECT_EQ(plain, "a , b");
  EXPECT_EQ(glyphs.size(), 2u);  // i-matra then ka
  EXPECT_EQ(out.rfind("a ", 0), 0u);
  EXPECT_EQ(out.substr(out.size() - 3), ", b");
}

TEST_F(ComplexShaperTest, MemoReusesRunsWithinAScope) {
  const char* word = kBengaliShaping[5].utf8;  // শকুন্তলা
  std::string first, second;
  ComplexShaper::beginMemo();
  const auto before = ComplexShaper::memoryStats();
  ASSERT_TRUE(shaper.shape(word, first));
  ComplexShaper::releaseCache();  // only the memo can serve the repeat now
  ASSERT_TRUE(shaper.shape(word, second));
  const auto after = ComplexShaper::memoryStats();
  ComplexShaper::endMemo();
  EXPECT_EQ(first, second);
  EXPECT_EQ(after.shapedRuns - before.shapedRuns, 1u);
  EXPECT_EQ(after.reusedRuns - before.reusedRuns, 1u);
}

TEST_F(ComplexShaperTest, MemoRepeatsAFailureUntilTheScopeEnds) {
  // Layout measures a word, then stores the form it draws: both passes must
  // agree even when memory frees up in between.
  const char* word = kBengaliShaping[1].utf8;
  std::string out;
  ComplexShaper::beginMemo();
  ComplexShaper::setMemoryBudget(16 * 1024);  // smaller than the layout tables
  EXPECT_FALSE(shaper.shape(word, out));
  ComplexShaper::setMemoryBudget(4 << 20);
  for (int i = 0; i < 100; i++) ASSERT_FALSE(shaper.shape(word, out));
  ComplexShaper::endMemo();

  bool shaped = false;
  for (int i = 0; i < 100 && !shaped; i++) shaped = shaper.shape(word, out);
  EXPECT_TRUE(shaped) << "outside the scope the shaper retries after its backoff";
}

TEST_F(ComplexShaperTest, SizesShareOneFace) {
  ComplexShaper larger;
  larger.setBlobSource(loadFixture, nullptr, kFixtureKey);
  larger.setScale(kFixturePpem26_6 * 2);
  std::string small, big;
  ASSERT_TRUE(shaper.shape(kBengaliShaping[0].utf8, small));
  ASSERT_TRUE(larger.shape(kBengaliShaping[0].utf8, big));
  EXPECT_EQ(gLoads, 1) << "the second size must reuse the first size's layout tables";
  const auto a = decode(small), b = decode(big);
  ASSERT_EQ(a.size(), b.size());
  for (size_t i = 0; i < a.size(); i++) {
    EXPECT_EQ(a[i].gid, b[i].gid);
    EXPECT_NEAR(b[i].advance12_4, 2 * a[i].advance12_4, 1);
  }
}

TEST_F(ComplexShaperTest, RefusesCleanlyWhenTheBudgetIsTooSmallAndRecovers) {
  ComplexShaper::setMemoryBudget(16 * 1024);  // smaller than the layout tables
  std::string out;
  EXPECT_FALSE(shaper.shape(kBengaliShaping[1].utf8, out));
  EXPECT_GT(ComplexShaper::memoryStats().failures, 0u);

  ComplexShaper::setMemoryBudget(4 << 20);
  bool shaped = false;
  for (int i = 0; i < 100 && !shaped; i++) shaped = shaper.shape(kBengaliShaping[1].utf8, out);
  ASSERT_TRUE(shaped) << "the shaper must retry after its backoff";
  EXPECT_EQ(decode(out).size(), kBengaliShaping[1].count);
}

// Fails its first `failuresLeft` loads, reporting them as the source being
// unavailable when `unavailable` is set (else they read as a malformed
// source), then loads the fixture.
struct FlakySource {
  int failuresLeft;
  bool unavailable;
};

bool loadFlakyBlob(void* ctx, ComplexShaper::Blob* out) {
  auto* source = static_cast<FlakySource*>(ctx);
  if (source->failuresLeft > 0) {
    source->failuresLeft--;
    if (source->unavailable) ComplexShaper::noteSourceUnavailable();
    return false;
  }
  return loadFixture(nullptr, out);
}

// The fixture's sfnt tables by tag, with GSUB failing as FlakySource says.
uint8_t* loadFlakyTable(void* ctx, const uint32_t tag, uint32_t* length) {
  auto* source = static_cast<FlakySource*>(ctx);
  const std::vector<uint8_t>& font = fixture();
  const auto u32 = [&](const size_t at) {
    return static_cast<uint32_t>(font[at] << 24 | font[at + 1] << 16 | font[at + 2] << 8 | font[at + 3]);
  };
  if (tag == 0x47535542u && source->failuresLeft > 0) {  // 'GSUB'
    source->failuresLeft--;
    if (source->unavailable) ComplexShaper::noteSourceUnavailable();
    return nullptr;
  }
  const unsigned count = font[4] << 8 | font[5];
  for (unsigned i = 0; i < count; i++) {
    const size_t record = 12 + 16 * i;
    if (u32(record) != tag) continue;
    *length = u32(record + 12);
    auto* table = static_cast<uint8_t*>(ComplexShaper::allocate(*length ? *length : 1));
    if (table != nullptr) std::memcpy(table, font.data() + u32(record + 8), *length);
    return table;
  }
  return nullptr;
}

bool shapesWithin(ComplexShaper& shaper, const int attempts, std::string& out,
                  const char* word = kBengaliShaping[1].utf8) {
  for (int i = 0; i < attempts; i++) {
    if (shaper.shape(word, out)) return true;
  }
  return false;
}

TEST_F(ComplexShaperTest, RetriesASourceThatCouldNotBeLoaded) {
  FlakySource source{2, true};
  ComplexShaper flaky;
  flaky.setBlobSource(loadFlakyBlob, &source, kFixtureKey + 300);
  flaky.setScale(kFixturePpem26_6);
  std::string out;
  ASSERT_TRUE(shapesWithin(flaky, 300, out)) << "an unreadable source is retried after the backoff";
  EXPECT_EQ(decode(out).size(), kBengaliShaping[1].count);
}

TEST_F(ComplexShaperTest, StopsLoadingAMalformedSource) {
  FlakySource source{1, false};
  ComplexShaper broken;
  broken.setBlobSource(loadFlakyBlob, &source, kFixtureKey + 301);
  broken.setScale(kFixturePpem26_6);
  std::string out;
  EXPECT_FALSE(shapesWithin(broken, 300, out));
}

TEST_F(ComplexShaperTest, DoesNotShapeWithoutATableThatFailedToLoad) {
  FlakySource source{1, true};
  ComplexShaper ttf;
  ttf.setTableSource(loadFlakyTable, &source);
  ttf.setScale(kFixturePpem26_6);
  const char* conjunct = kBengaliShaping[5].utf8;  // শকুন্তলা: GSUB forms its conjunct
  std::string out, expected;
  ASSERT_TRUE(shapesWithin(ttf, 300, out, conjunct));
  ASSERT_TRUE(shaper.shape(conjunct, expected));
  EXPECT_EQ(decode(out), decode(expected)) << "shaped with every layout table";
}

// The fixture's hmtx advances at the fixture scale, plus one pixel.
int32_t widerAdvance(void* ctx, const uint32_t glyph) {
  const auto* face = static_cast<const ot::Face*>(ctx);
  ot::Scale scale;
  scale.set(static_cast<int32_t>(kFixturePpem26_6), (kFixturePpem26_6 + 32) >> 6, face->upem());
  return scale.emScaleX(face->advance(glyph)) + 64;
}

TEST_F(ComplexShaperTest, AdvancesByTheAdvanceSourceKeepingGposAdjustments) {
  ot::FaceTables tables;
  ASSERT_TRUE(ot::tablesFromSfnt(fixture().data(), static_cast<uint32_t>(fixture().size()), &tables));
  ot::Face face;
  ASSERT_TRUE(face.init(tables));
  const char* word = kBengaliShaping[5].utf8;
  std::string base, wider;
  ASSERT_TRUE(shaper.shape(word, base));
  shaper.setAdvanceSource(widerAdvance, &face);
  ASSERT_TRUE(shaper.shape(word, wider));
  const auto a = decode(base), b = decode(wider);
  ASSERT_EQ(a.size(), b.size());
  int widened = 0;
  for (size_t i = 0; i < a.size(); i++) {
    EXPECT_EQ(a[i].gid, b[i].gid);
    if (a[i].advance12_4 == 0) continue;  // a zeroed mark keeps no advance
    EXPECT_EQ(b[i].advance12_4, a[i].advance12_4 + 16) << "glyph " << i;
    widened++;
  }
  EXPECT_GT(widened, 0);
  shaper.setAdvanceSource(nullptr, nullptr);
  std::string again;
  ASSERT_TRUE(shaper.shape(word, again));
  EXPECT_EQ(again, base);
}

namespace {

// HarfBuzz keeps an empty table in place of one it could not allocate, and
// shapes with it afterwards without reporting an error. However memory runs
// short, a successful shape() must match the reference, both then and once
// memory is back.
void expectShapesCorrectlyUnderPressure(ComplexShaper& shaper, const std::string& when,
                                        const std::function<void()>& restoreMemory) {
  const auto& word = kBengaliShaping[5];   // শকুন্তলা: a conjunct and positioned marks
  const auto& later = kBengaliShaping[6];  // প্রকাশ: first shaped once memory is back
  std::string out;
  for (int call = 0; call < 3; call++) {
    if (shaper.shape(word.utf8, out)) ASSERT_EQ(glyphIds(out), glyphIds(word)) << when << ", call " << call;
  }
  restoreMemory();
  for (const auto* entry : {&word, &later}) {
    bool shaped = false;
    for (int i = 0; i < 100 && !shaped; i++) shaped = shaper.shape(entry->utf8, out);
    ASSERT_TRUE(shaped) << when;
    ASSERT_EQ(glyphIds(out), glyphIds(*entry)) << when << ", after recovery";
  }
}

}  // namespace

TEST_F(ComplexShaperTest, RecoversFromARefusedAllocationAnywhere) {
  int64_t allocations = 0;  // counted by the first pass, which refuses nothing
  for (int64_t refused = -1; refused < allocations; refused++) {
    ComplexShaper::releaseAll();
    ComplexShaper probe;
    probe.setBlobSource(loadFixture, nullptr, kFixtureKey + 400);
    probe.setScale(kFixturePpem26_6);
    ESP.freeHeapCalls = 0;
    ESP.emptyAtCall = refused;
    expectShapesCorrectlyUnderPressure(probe, "allocation " + std::to_string(refused) + " refused", [&] {
      if (refused < 0) allocations = ESP.freeHeapCalls;
      ESP.emptyAtCall = -1;
    });
    if (HasFatalFailure()) return;
  }
  EXPECT_GT(allocations, 0);
}

TEST_F(ComplexShaperTest, RecoversFromEveryBudgetShortfall) {
  // A nearly spent budget admits small allocations and refuses larger ones,
  // so the face builds and a table inside it does not.
  for (size_t budget = fixture().size(); budget < fixture().size() + 48 * 1024; budget += 128) {
    ComplexShaper::releaseAll();
    ComplexShaper::setMemoryBudget(budget);
    ComplexShaper probe;
    probe.setBlobSource(loadFixture, nullptr, kFixtureKey + 400);
    probe.setScale(kFixturePpem26_6);
    expectShapesCorrectlyUnderPressure(probe, "budget " + std::to_string(budget),
                                       [] { ComplexShaper::setMemoryBudget(4 << 20); });
    if (HasFatalFailure()) return;
  }
}

TEST_F(ComplexShaperTest, RetriesTheNextWordOnceTheBackoffHasLasted) {
  // Pages drawn from the page cache do not shape, so a shaper can sit unused
  // while memory comes back; its next word must not wait out the call count.
  const auto& word = kBengaliShaping[1];
  std::string out;
  ComplexShaper::setMemoryBudget(16 * 1024);  // smaller than the layout tables
  EXPECT_FALSE(shaper.shape(word.utf8, out));
  ComplexShaper::setMemoryBudget(4 << 20);
  EXPECT_FALSE(shaper.shape(word.utf8, out)) << "a retry right away would re-read the source for every word";
  hostMillis += 1000;
  ASSERT_TRUE(shaper.shape(word.utf8, out));
  EXPECT_EQ(glyphIds(out), glyphIds(word));
}

TEST_F(ComplexShaperTest, MakesRoomForANewFaceWhenTheBudgetHoldsOnlyOne) {
  // Two faces of the Bengali tables (distinct keys, so not shared) do not fit
  // together; the second shaper must drop the first face rather than give up.
  std::string out;
  ASSERT_TRUE(shaper.shape(kBengaliShaping[1].utf8, out));
  const size_t oneFace = ComplexShaper::memoryStats().current;
  ComplexShaper::setMemoryBudget(oneFace + oneFace / 2);
  ComplexShaper other;
  other.setBlobSource(loadFixture, nullptr, kFixtureKey + 200);
  other.setScale(kFixturePpem26_6);
  ASSERT_TRUE(other.shape(kBengaliShaping[2].utf8, out)) << "the first check of a new font must make room too";
  EXPECT_EQ(decode(out).size(), kBengaliShaping[2].count);
}

TEST_F(ComplexShaperTest, ReleaseFreesEverythingAndRebuilds) {
  std::string out;
  ASSERT_TRUE(shaper.shape(kBengaliShaping[2].utf8, out));
  EXPECT_GT(ComplexShaper::memoryStats().current, fixture().size());
  ComplexShaper::releaseAll();
  EXPECT_EQ(ComplexShaper::memoryStats().current, 0u);
  std::string again;
  ASSERT_TRUE(shaper.shape(kBengaliShaping[2].utf8, again));
  EXPECT_EQ(out, again);
}
