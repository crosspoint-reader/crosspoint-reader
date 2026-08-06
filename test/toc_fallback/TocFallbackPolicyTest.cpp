#include <gtest/gtest.h>

#include "Epub/TocFallbackPolicy.h"

using toc_fallback::navIsSparse;
using toc_fallback::shouldAdoptNcx;

TEST(NavIsSparse, NotParsedIsNeverSparse) {
  EXPECT_FALSE(navIsSparse(false, 0, 20));
  EXPECT_FALSE(navIsSparse(false, 5, 20));
}

TEST(NavIsSparse, MappedUnderHalfSpineIsSparse) {
  EXPECT_TRUE(navIsSparse(true, 5, 11));
  EXPECT_TRUE(navIsSparse(true, 5, 20));
}

TEST(NavIsSparse, MappedAtHalfSpineIsNotSparse) {
  EXPECT_FALSE(navIsSparse(true, 5, 10));
  EXPECT_FALSE(navIsSparse(true, 10, 20));
}

// A nav with many raw entries but few resolving to the spine is functionally
// sparse: only the mapped count matters (here e.g. 20 raw, 3 mapped).
TEST(NavIsSparse, DeadLinkNavIsSparse) { EXPECT_TRUE(navIsSparse(true, 3, 20)); }

TEST(NavIsSparse, ZeroMappedNonEmptySpineIsSparse) { EXPECT_TRUE(navIsSparse(true, 0, 8)); }

TEST(NavIsSparse, EmptySpineIsNotSparse) { EXPECT_FALSE(navIsSparse(true, 0, 0)); }

// Regression: a stale NCX whose hrefs no longer resolve to the spine (8 raw
// entries, 0 mapped) must not supersede a valid sparse nav (5 entries, all
// mapped). Comparing raw counts here adopted the NCX, and every chapter
// selection then silently cancelled on spineIndex == -1.
TEST(ShouldAdoptNcx, StaleNcxDoesNotSupersedeMappedNav) { EXPECT_FALSE(shouldAdoptNcx(true, 5, true, 0)); }

// The original purpose of the fallback still works: an NCX resolving far more
// of the spine than a coarse nav wins.
TEST(ShouldAdoptNcx, RicherNcxSupersedesSparseNav) { EXPECT_TRUE(shouldAdoptNcx(true, 2, true, 12)); }

TEST(ShouldAdoptNcx, TieKeepsNav) { EXPECT_FALSE(shouldAdoptNcx(true, 5, true, 5)); }

TEST(ShouldAdoptNcx, FewerMappedKeepsNav) { EXPECT_FALSE(shouldAdoptNcx(true, 5, true, 4)); }

TEST(ShouldAdoptNcx, NcxParseFailureKeepsNav) { EXPECT_FALSE(shouldAdoptNcx(true, 5, false, 0)); }

// Without a parsed nav, any parsed NCX is adopted even if nothing mapped:
// unmapped titles still display, which beats no TOC at all.
TEST(ShouldAdoptNcx, NavUnparsedAdoptsAnyParsedNcx) { EXPECT_TRUE(shouldAdoptNcx(false, 0, true, 0)); }

TEST(ShouldAdoptNcx, NeitherParsedAdoptsNothing) { EXPECT_FALSE(shouldAdoptNcx(false, 0, false, 0)); }

TEST(ShouldAdoptNcx, BothZeroMappedKeepsNav) { EXPECT_FALSE(shouldAdoptNcx(true, 0, true, 0)); }
