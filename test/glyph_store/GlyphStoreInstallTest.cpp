// Installing a family into the glyph store and opening it.
#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

#include "GlyphStoreTestUtil.h"

using namespace glyphstore;
using namespace gst_test;

namespace {

class GlyphStoreInstall : public StoreTest {};

Layout twoStyleLayout(const uint8_t mode) {
  Layout layout = smallLayout();
  layout.geometry.mode = mode;
  layout.geometry.styleCount = 2;
  layout.styleOffset[1] = layout.styleOffset[0] + 4096;
  layout.styleSize[1] = 4096;
  return layout;
}

}  // namespace

TEST_F(GlyphStoreInstall, ABlankChipIsNotInstalled) { EXPECT_EQ(OpenResult::NotInstalled, store.open(kIdentityA)); }

TEST_F(GlyphStoreInstall, AnInstalledStoreOpensEmpty) {
  ASSERT_TRUE(install(store, smallLayout()));
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  EXPECT_TRUE(store.isOpen());
  EXPECT_EQ(kModeFlash, store.mode());
  EXPECT_EQ(1, store.styleCount());
  EXPECT_EQ(0u, store.logUsed());
  EXPECT_EQ(0u, store.usedSlots());
  EXPECT_EQ(0u, store.usedPairSlots());
  EXPECT_FALSE(store.wipePending());
  EXPECT_NE(nullptr, store.logBase());
}

TEST_F(GlyphStoreInstall, AnotherFamilyIsReported) {
  ASSERT_TRUE(install(store, smallLayout(), kIdentityA));
  EXPECT_EQ(OpenResult::OtherFamily, store.open(kIdentityB));
  EXPECT_FALSE(store.isOpen());
  EXPECT_EQ(0u, flash.mappings());
}

TEST_F(GlyphStoreInstall, StyleDataIsReadableThroughTheMapping) {
  ASSERT_TRUE(installAndOpen(store, smallLayout()));
  const uint8_t* style = store.mapStyle(0);
  ASSERT_NE(nullptr, style);
  for (uint32_t i = 0; i < 4096; ++i) ASSERT_EQ(styleByte(0, i), style[i]) << i;
  EXPECT_EQ(nullptr, store.mapStyle(1));
}

TEST_F(GlyphStoreInstall, UntilFinishTheStoreIsNotInstalled) {
  const Layout layout = smallLayout();
  ASSERT_TRUE(store.beginInstall(layout, kIdentityA, nullptr));
  const std::vector<uint8_t> data(layout.styleSize[0], 0x11);
  ASSERT_TRUE(store.writeStyleData(0, 0, data.data(), static_cast<uint32_t>(data.size())));
  {
    FlashGlyphStore other(flash);
    EXPECT_EQ(OpenResult::NotInstalled, other.open(kIdentityA));
  }
  ASSERT_TRUE(store.finishInstall());
  EXPECT_EQ(OpenResult::Ready, store.open(kIdentityA));
}

TEST_F(GlyphStoreInstall, AnInstallAbandonedByOpenCannotBeFinishedLater) {
  ASSERT_TRUE(store.beginInstall(smallLayout(), kIdentityA, nullptr));
  EXPECT_EQ(OpenResult::NotInstalled, store.open(kIdentityA));
  EXPECT_FALSE(store.finishInstall());  // the half-done install was dropped, not left to write a stale header
}

TEST(GlyphStoreInstallCut, ACutAnywhereInTheHeaderWriteReadsAsNotInstalled) {
  const Layout layout = smallLayout();
  for (const bool partial : {false, true}) {
    for (uint32_t cut = 0; cut < sizeof(Header); ++cut) {
      SimNorFlash flash(kFlashBytes);
      FlashGlyphStore store(flash);
      ASSERT_TRUE(store.beginInstall(layout, kIdentityA, nullptr));
      flash.partialCut(partial);
      flash.cutAfterSteps(cut);
      EXPECT_FALSE(store.finishInstall());
      flash.powerCycle();
      FlashGlyphStore reopened(flash);
      const OpenResult result = reopened.open(kIdentityA);
      // Only a partly programmed last byte can complete the header by chance.
      const bool lastByteByChance = partial && cut == sizeof(Header) - 1 && result == OpenResult::Ready;
      EXPECT_TRUE(result == OpenResult::NotInstalled || lastByteByChance) << "cut " << cut << " partial " << partial;
    }
  }
}

TEST_F(GlyphStoreInstall, InstallingOnABlankChipErasesNothing) {
  ASSERT_TRUE(installAndOpen(store, smallLayout()));
  EXPECT_EQ(0u, flash.eraseCalls());
}

TEST_F(GlyphStoreInstall, OpenFailsCleanlyWhenTheRegionsCannotBeMapped) {
  ASSERT_TRUE(install(store, smallLayout()));
  flash.setFreePages(0);
  EXPECT_EQ(OpenResult::MapFailed, store.open(kIdentityA));
  EXPECT_FALSE(store.isOpen());
  EXPECT_EQ(0u, flash.mappings());
  flash.setFreePages(32);
  EXPECT_EQ(OpenResult::Ready, store.open(kIdentityA));
}

TEST_F(GlyphStoreInstall, ALayoutOutsideTheChipIsRefused) {
  Layout tooLong = smallLayout();
  tooLong.geometry.logSize = kFlashBytes;  // runs past the end
  EXPECT_FALSE(store.beginInstall(tooLong, kIdentityA, nullptr));
  Layout overlapping = smallLayout();
  overlapping.styleSize[0] = 70000;  // into the index
  EXPECT_FALSE(store.beginInstall(overlapping, kIdentityA, nullptr));
  Layout tinyIndex = smallLayout(kMaxProbe - 1);  // a probe could wrap onto itself
  EXPECT_FALSE(store.beginInstall(tinyIndex, kIdentityA, nullptr));
  EXPECT_EQ(0u, flash.eraseCalls());
  EXPECT_EQ(0u, flash.programCalls());
}

TEST_F(GlyphStoreInstall, SdModeNeedsTablesThatFitTheirStyle) {
  const Layout layout = twoStyleLayout(kModeSd);
  EXPECT_FALSE(store.beginInstall(layout, kIdentityA, nullptr));  // no table maps
  StyleInfo info[kMaxStyles] = {};
  info[1].tableCount = 1;
  info[1].tables[0] = {0x636D6170, 0, 4000, 200};  // 'cmap', past the 4 KB block
  EXPECT_FALSE(store.beginInstall(layout, kIdentityA, info));
  info[1].tableCount = kMaxTables + 1;
  info[1].tables[0].blockOffset = 0;
  EXPECT_FALSE(store.beginInstall(layout, kIdentityA, info));
}

TEST_F(GlyphStoreInstall, SdModeMapsAllStylesTogether) {
  const Layout layout = twoStyleLayout(kModeSd);
  StyleInfo info[kMaxStyles] = {};
  info[1].sdPathHash = 0xCAFE;
  info[1].fileSize = 19700000;
  info[1].tableCount = 1;
  info[1].tables[0] = {0x636D6170, 1234, 0, 4096};  // 'cmap'
  ASSERT_TRUE(install(store, layout, kIdentityA, info));
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  const uint8_t* regular = store.mapStyle(0);
  const uint8_t* italic = store.mapStyle(1);
  ASSERT_NE(nullptr, regular);
  ASSERT_NE(nullptr, italic);
  EXPECT_EQ(styleByte(0, 5), regular[5]);  // the first pointer is still valid
  EXPECT_EQ(styleByte(1, 5), italic[5]);
  EXPECT_EQ(2u, flash.mappings());  // the glyph regions, and one mapping for both styles
  StyleInfo read{};
  ASSERT_TRUE(store.styleInfo(1, read));
  EXPECT_EQ(0xCAFEu, read.sdPathHash);
  EXPECT_EQ(1u, read.tableCount);
  EXPECT_EQ(1234u, read.tables[0].fileOffset);
  EXPECT_EQ(layout.styleOffset[1], read.offset);
}

TEST_F(GlyphStoreInstall, FlashModeMapsOneStyleAtATime) {
  ASSERT_TRUE(install(store, twoStyleLayout(kModeFlash)));
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  ASSERT_NE(nullptr, store.mapStyle(0));
  const uint8_t* italic = store.mapStyle(1);
  ASSERT_NE(nullptr, italic);
  EXPECT_EQ(styleByte(1, 9), italic[9]);
  EXPECT_EQ(2u, flash.mappings());  // the glyph regions and the one style
  store.close();
  EXPECT_EQ(0u, flash.mappings());
}

TEST_F(GlyphStoreInstall, AHeaderThatPassesItsCheckButDescribesOverlappingStylesIsNotInstalled) {
  ASSERT_TRUE(install(store, twoStyleLayout(kModeFlash)));
  Header header{};
  ASSERT_TRUE(flash.read(0, &header, sizeof header));
  header.styles[0].size = 8192;  // now runs into style 1
  header.check = fnv1a(&header, offsetof(Header, check));
  ASSERT_TRUE(flash.erase(0, kSectorBytes));
  ASSERT_TRUE(flash.program(0, &header, sizeof header));
  EXPECT_EQ(OpenResult::NotInstalled, store.open(kIdentityA));
}
