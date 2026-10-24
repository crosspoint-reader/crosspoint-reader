#include <Epub/Epub/PageLink.h>
#include <gtest/gtest.h>

#include <cstring>
#include <new>

TEST(PageLinkTest, HitAreaIncludesFingerSlopAndMinimumWidth) {
  PageLink link;
  link.x = 100;
  link.y = 50;
  link.width = 8;
  link.height = 20;

  EXPECT_TRUE(link.contains(90, 44, 6, 28));
  EXPECT_TRUE(link.contains(117, 75, 6, 28));
  EXPECT_FALSE(link.contains(89, 50, 6, 28));
  EXPECT_FALSE(link.contains(118, 50, 6, 28));
  EXPECT_FALSE(link.contains(100, 76, 6, 28));
}

TEST(PageLinkTest, ConstructionZeroesTheWholeHrefBuffer) {
  // Page::serialize writes all FOOTNOTE_HREF_LEN bytes of href, while addLink copies only the
  // string. Bytes past the terminator must not carry whatever the allocation held before.
  alignas(PageLink) unsigned char storage[sizeof(PageLink)];
  std::memset(storage, 0xAA, sizeof(storage));
  auto* link = new (storage) PageLink();
  for (size_t i = 0; i < sizeof(link->href); ++i) {
    EXPECT_EQ(link->href[i], '\0') << "byte " << i;
  }
  link->~PageLink();
}
