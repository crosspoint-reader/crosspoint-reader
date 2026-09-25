#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ComplexShaper.h"
#include "ExpectedShaping.h"
#include "ShapingTokens.h"
#include "Utf8.h"

namespace {

std::vector<uint8_t> readFixture() {
  std::vector<uint8_t> bytes;
  if (FILE* f = std::fopen(SHAPING_FIXTURE, "rb")) {
    uint8_t buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
    std::fclose(f);
  }
  return bytes;
}

const std::vector<uint8_t>& fixture() {
  static const std::vector<uint8_t> bytes = readFixture();
  return bytes;
}

int gLoads = 0;

bool loadFixture(void*, ComplexShaper::Blob* out) {
  gLoads++;
  const auto& bytes = fixture();
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

class ComplexShaperTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_FALSE(fixture().empty()) << "missing " << SHAPING_FIXTURE;
    ComplexShaper::setMemoryBudget(4 << 20);
    shaper.setBlobSource(loadFixture, nullptr, kFixtureKey);
    shaper.setScale(kFixturePpem26_6);
    gLoads = 0;
  }
  void TearDown() override { ComplexShaper::releaseAll(); }

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

TEST_F(ComplexShaperTest, LeavesTextWithoutBengaliAlone) {
  std::string out;
  EXPECT_FALSE(shaper.shape("hello world", out));
  EXPECT_FALSE(shaper.shape("\xE0\xA5\xA4", out));  // a lone danda
  EXPECT_EQ(gLoads, 0) << "no layout tables should load for text that needs no shaping";
}

TEST_F(ComplexShaperTest, MatchesHarfBuzzReferenceShaping) {
  for (const auto& expected : kExpectedShaping) {
    std::string out;
    ASSERT_TRUE(shaper.shape(expected.utf8, out)) << expected.utf8;
    std::vector<DecodedGlyph> want;
    for (uint8_t i = 0; i < expected.count; i++) {
      const ExpectedGlyph& g = expected.glyphs[i];
      want.push_back({g.gid, g.advance12_4, g.dx, g.dy});
    }
    EXPECT_EQ(decode(out), want) << expected.utf8;
  }
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
  const char* word = kExpectedShaping[5].utf8;  // শকুন্তলা
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

TEST_F(ComplexShaperTest, SizesShareOneFace) {
  ComplexShaper larger;
  larger.setBlobSource(loadFixture, nullptr, kFixtureKey);
  larger.setScale(kFixturePpem26_6 * 2);
  std::string small, big;
  ASSERT_TRUE(shaper.shape(kExpectedShaping[0].utf8, small));
  ASSERT_TRUE(larger.shape(kExpectedShaping[0].utf8, big));
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
  EXPECT_FALSE(shaper.shape(kExpectedShaping[1].utf8, out));
  EXPECT_GT(ComplexShaper::memoryStats().failures, 0u);

  ComplexShaper::setMemoryBudget(4 << 20);
  bool shaped = false;
  for (int i = 0; i < 100 && !shaped; i++) shaped = shaper.shape(kExpectedShaping[1].utf8, out);
  ASSERT_TRUE(shaped) << "the shaper must retry after its backoff";
  EXPECT_EQ(decode(out).size(), kExpectedShaping[1].count);
}

TEST_F(ComplexShaperTest, ReleaseFreesEverythingAndRebuilds) {
  std::string out;
  ASSERT_TRUE(shaper.shape(kExpectedShaping[2].utf8, out));
  EXPECT_GT(ComplexShaper::memoryStats().current, fixture().size());
  ComplexShaper::releaseAll();
  // HarfBuzz keeps a few hundred bytes of process-wide statics (language list).
  EXPECT_LT(ComplexShaper::memoryStats().current, 1024u);
  std::string again;
  ASSERT_TRUE(shaper.shape(kExpectedShaping[2].utf8, again));
  EXPECT_EQ(out, again);
}
