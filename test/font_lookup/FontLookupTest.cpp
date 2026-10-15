// The renderer's single font lookup: a hit is a plain map hit, a miss may ask a resolver once.
#include <gtest/gtest.h>

#include <map>
#include <optional>

#include "FontLookup.h"

namespace {

struct Resolver {
  int calls = 0;
  std::optional<ResolvedFont<int>> answer;

  static std::optional<ResolvedFont<int>> resolve(void* ctx, const int fontId) {
    auto* self = static_cast<Resolver*>(ctx);
    self->calls++;
    (void)fontId;
    return self->answer;
  }
  FontResolverHook<int> hook() { return FontResolverHook<int>{&Resolver::resolve, this}; }
};

}  // namespace

TEST(FontLookup, HitDoesNotAskTheResolver) {
  std::map<int, int> fonts{{7, 70}};
  std::map<int, float> scales;
  Resolver r;
  r.answer = ResolvedFont<int>{99};

  const int* found = findOrResolveFont(fonts, scales, 7, r.hook());

  ASSERT_NE(nullptr, found);
  EXPECT_EQ(70, *found);
  EXPECT_EQ(0, r.calls);
}

TEST(FontLookup, MissWithoutResolverReturnsNull) {
  std::map<int, int> fonts{{7, 70}};
  std::map<int, float> scales;

  EXPECT_EQ(nullptr, findOrResolveFont(fonts, scales, 8, FontResolverHook<int>{}));
  EXPECT_EQ(1u, fonts.size());
}

TEST(FontLookup, MissAsksTheResolverAndRegistersWhatItSupplies) {
  std::map<int, int> fonts;
  std::map<int, float> scales;
  Resolver r;
  r.answer = ResolvedFont<int>{42};

  const int* first = findOrResolveFont(fonts, scales, 8, r.hook());
  const int* second = findOrResolveFont(fonts, scales, 8, r.hook());

  ASSERT_NE(nullptr, first);
  EXPECT_EQ(42, *first);
  EXPECT_EQ(first, second);  // the second lookup is a plain hit on the registered entry
  EXPECT_EQ(1, r.calls);
  EXPECT_TRUE(scales.empty());  // drawn at its own size
}

TEST(FontLookup, ResolverIsAskedOncePerMiss) {
  std::map<int, int> fonts;
  std::map<int, float> scales;
  Resolver r;  // declines

  EXPECT_EQ(nullptr, findOrResolveFont(fonts, scales, 8, r.hook()));
  EXPECT_EQ(1, r.calls);
}

TEST(FontLookup, DeclinedIdIsNotRegistered) {
  std::map<int, int> fonts{{7, 70}};
  std::map<int, float> scales;
  Resolver r;  // declines

  findOrResolveFont(fonts, scales, 8, r.hook());

  EXPECT_EQ(1u, fonts.size());
  EXPECT_EQ(0u, fonts.count(8));
}

// A family standing in for another size (the size it names could not be made) comes with the scale that keeps the
// text at the named size, as a scaled font registered up front would.
TEST(FontLookup, AStandInIsRegisteredWithItsScale) {
  std::map<int, int> fonts;
  std::map<int, float> scales;
  Resolver r;
  r.answer = ResolvedFont<int>{47, 1.25f};

  ASSERT_NE(nullptr, findOrResolveFont(fonts, scales, 8, r.hook()));

  EXPECT_FLOAT_EQ(1.25f, scales.at(8));
}

// Layout often asks for a font's scale before anything else about it: the scale of an ID nobody looked up yet
// resolves it first, so the first measurement already uses the stand-in's scale.
TEST(FontLookup, TheScaleOfAnIdNotYetLookedUpResolvesItFirst) {
  std::map<int, int> fonts{{7, 70}};
  std::map<int, float> scales{{7, 1.1f}};
  Resolver r;
  r.answer = ResolvedFont<int>{47, 1.25f};

  EXPECT_FLOAT_EQ(1.25f, resolvedBaseScale(fonts, scales, 8, r.hook()));
  EXPECT_EQ(1u, fonts.count(8));
  EXPECT_FLOAT_EQ(1.25f, resolvedBaseScale(fonts, scales, 8, r.hook()));
  EXPECT_EQ(1, r.calls);
  EXPECT_FLOAT_EQ(1.1f, resolvedBaseScale(fonts, scales, 7, r.hook()));  // known fonts: the map alone
  EXPECT_FLOAT_EQ(1.0f, resolvedBaseScale(fonts, scales, 9, FontResolverHook<int>{}));
  EXPECT_EQ(1, r.calls);
}
