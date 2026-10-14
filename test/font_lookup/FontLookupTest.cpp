// The renderer's single font lookup: a hit is a plain map hit, a miss may ask a resolver once.
#include <gtest/gtest.h>

#include <map>
#include <optional>

#include "FontLookup.h"

namespace {

struct Resolver {
  int calls = 0;
  std::optional<int> answer;

  static std::optional<int> resolve(void* ctx, const int fontId) {
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
  Resolver r;
  r.answer = 99;

  const int* found = findOrResolveFont(fonts, 7, r.hook());

  ASSERT_NE(nullptr, found);
  EXPECT_EQ(70, *found);
  EXPECT_EQ(0, r.calls);
}

TEST(FontLookup, MissWithoutResolverReturnsNull) {
  std::map<int, int> fonts{{7, 70}};

  EXPECT_EQ(nullptr, findOrResolveFont(fonts, 8, FontResolverHook<int>{}));
  EXPECT_EQ(1u, fonts.size());
}

TEST(FontLookup, MissAsksTheResolverAndRegistersWhatItSupplies) {
  std::map<int, int> fonts;
  Resolver r;
  r.answer = 42;

  const int* first = findOrResolveFont(fonts, 8, r.hook());
  const int* second = findOrResolveFont(fonts, 8, r.hook());

  ASSERT_NE(nullptr, first);
  EXPECT_EQ(42, *first);
  EXPECT_EQ(first, second);  // the second lookup is a plain hit on the registered entry
  EXPECT_EQ(1, r.calls);
}

TEST(FontLookup, ResolverIsAskedOncePerMiss) {
  std::map<int, int> fonts;
  Resolver r;  // declines

  EXPECT_EQ(nullptr, findOrResolveFont(fonts, 8, r.hook()));
  EXPECT_EQ(1, r.calls);
}

TEST(FontLookup, DeclinedIdIsNotRegistered) {
  std::map<int, int> fonts{{7, 70}};
  Resolver r;  // declines

  findOrResolveFont(fonts, 8, r.hook());

  EXPECT_EQ(1u, fonts.size());
  EXPECT_EQ(0u, fonts.count(8));
}
