// Installing a family into the glyph store and opening it.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>
#include <utility>
#include <vector>

#include "GlyphStoreTestUtil.h"

using namespace glyphstore;
using namespace gst_test;

namespace {

class GlyphStoreInstall : public StoreTest {};

// Two 4 KB style blocks: style 0 in `first` mode, style 1 in `second`.
Layout twoStyleLayout(const uint8_t first, const uint8_t second) {
  Layout layout = smallLayout();
  layout.geometry.styleCount = 2;
  layout.styleOffset[1] = layout.styleOffset[0] + 4096;
  layout.styleSize[1] = 4096;
  layout.styleMode[0] = first;
  layout.styleMode[1] = second;
  return layout;
}

Layout twoStyleLayout(const uint8_t mode) { return twoStyleLayout(mode, mode); }

// An SD-mode style's record: its file on the card, and its outlines there, past the 4 KB block.
StyleInfo sdStyle(const uint32_t stamp = 0xCAFE) {
  StyleInfo info{};
  info.fileStamp = stamp;
  info.fileSize = 19700000;
  info.outlines = {356736, 4096, 3576385};
  return info;
}

}  // namespace

TEST_F(GlyphStoreInstall, ABlankChipIsNotInstalled) { EXPECT_EQ(OpenResult::NotInstalled, store.open(kIdentityA)); }

TEST_F(GlyphStoreInstall, AnInstalledStoreOpensEmpty) {
  ASSERT_TRUE(install(store, smallLayout()));
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  EXPECT_TRUE(store.isOpen());
  EXPECT_EQ(1, store.styleCount());
  StyleInfo info{};
  ASSERT_TRUE(store.styleInfo(0, info));
  EXPECT_EQ(kModeFlash, info.mode);
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

TEST_F(GlyphStoreInstall, AnSdModeStyleNeedsItsOutlinesInItsFileAndPastItsBlock) {
  const Layout layout = twoStyleLayout(kModeSd);
  EXPECT_FALSE(store.beginInstall(layout, kIdentityA, nullptr));  // no files
  StyleInfo info[kMaxStyles] = {sdStyle(), sdStyle()};
  info[1].outlines.fontOffset = 4000;  // inside the block, where its tables are
  EXPECT_FALSE(store.beginInstall(layout, kIdentityA, info));
  info[1] = sdStyle();
  info[1].fileSize = info[1].outlines.fileOffset + info[1].outlines.length - 1;  // past the end of the file
  EXPECT_FALSE(store.beginInstall(layout, kIdentityA, info));
  info[1] = sdStyle();
  info[1].outlines.fontOffset = 0xFFFFFF00u;  // its end would wrap
  EXPECT_FALSE(store.beginInstall(layout, kIdentityA, info));
  EXPECT_EQ(0u, flash.eraseCalls());
  EXPECT_EQ(0u, flash.programCalls());
}

TEST_F(GlyphStoreInstall, SdModeBlocksComeAfterEveryFlashModeBlock) {
  StyleInfo info[kMaxStyles] = {sdStyle(), sdStyle()};
  EXPECT_FALSE(store.beginInstall(twoStyleLayout(kModeSd, kModeFlash), kIdentityA, info));
  EXPECT_FALSE(store.beginInstall(twoStyleLayout(kModeFlash, 2), kIdentityA, info));  // no such mode
  EXPECT_TRUE(store.beginInstall(twoStyleLayout(kModeFlash, kModeSd), kIdentityA, info));
}

TEST_F(GlyphStoreInstall, SdModeMapsAllStylesTogether) {
  const Layout layout = twoStyleLayout(kModeSd);
  StyleInfo info[kMaxStyles] = {sdStyle(), sdStyle(0xCAFE)};
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
  EXPECT_EQ(kModeSd, read.mode);
  EXPECT_EQ(0xCAFEu, read.fileStamp);
  EXPECT_EQ(19700000u, read.fileSize);
  EXPECT_EQ(356736u, read.outlines.fileOffset);
  EXPECT_EQ(4096u, read.outlines.fontOffset);
  EXPECT_EQ(3576385u, read.outlines.length);
  EXPECT_EQ(layout.styleOffset[1], read.offset);
}

TEST_F(GlyphStoreInstall, AFlashModeStyleMapsAloneBesideTheSdModeOnes) {
  // A body in flash mode and an accent in SD mode (spec 2c-4): the body's block maps on its own, one flash-mode
  // style at a time; the accent's tables map beside it and stay.
  Layout layout = twoStyleLayout(kModeFlash, kModeSd);
  layout.geometry.styleCount = 3;
  layout.styleOffset[1] = layout.styleOffset[0] + 4096;
  layout.styleMode[1] = kModeFlash;
  layout.styleOffset[2] = layout.styleOffset[1] + 4096;
  layout.styleSize[2] = 4096;
  layout.styleMode[2] = kModeSd;
  StyleInfo info[kMaxStyles] = {{}, {}, sdStyle()};
  ASSERT_TRUE(install(store, layout, kIdentityA, info));
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  // The SD-mode tables map at open, beside the regions and before any flash-mode block: they stay, and a flash-mode
  // block mapped after them leaves, when it goes, a gap joined to the free pages past it.
  EXPECT_EQ(2u, flash.mappings());
  const uint8_t* accent = store.mapStyle(2);
  ASSERT_NE(nullptr, store.mapStyle(0));
  const uint8_t* bold = store.mapStyle(1);
  ASSERT_NE(nullptr, bold);
  EXPECT_EQ(styleByte(1, 9), bold[9]);
  EXPECT_EQ(styleByte(2, 9), accent[9]);  // still mapped
  EXPECT_EQ(3u, flash.mappings());        // the glyph regions, one flash-mode style, the SD-mode tables
  StyleInfo read{};
  ASSERT_TRUE(store.styleInfo(1, read));
  EXPECT_EQ(kModeFlash, read.mode);
  EXPECT_EQ(0u, read.fileStamp);
  store.close();
  EXPECT_EQ(0u, flash.mappings());
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

namespace {

// An older header: its first fields as this version has them, with that version's number and size (version 1: one
// family's four style blocks, 648 B; version 2: eight blocks with table maps, 1,240 B), its check valid.
struct OldHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t headerBytes;
  uint8_t identity[kIdentityBytes];
  Geometry geometry;
  StyleInfo styles[1];
  uint32_t check;
};

}  // namespace

TEST(GlyphStoreOlderVersions, AnOlderStoreIsInstalledOverAndHoldsNothingOfIt) {
  for (const auto& [version, bytes] : {std::pair<uint16_t, uint16_t>{1, 648}, {2, 1240}}) {
    // What older firmware left: a header, a face (size) table entry, and a glyph with its slot.
    SimNorFlash flash(kFlashBytes);
    FlashGlyphStore store(flash);
    const Layout layout = smallLayout();
    OldHeader old{};
    old.magic = kMagic;
    old.version = version;
    old.headerBytes = bytes;
    std::memcpy(old.identity, kIdentityA, kIdentityBytes);
    old.geometry = layout.geometry;
    old.styles[0].offset = layout.styleOffset[0];
    old.styles[0].size = layout.styleSize[0];
    old.check = fnv1a(&old, offsetof(OldHeader, check));
    ASSERT_TRUE(flash.program(0, &old, sizeof old));
    const FaceEntry size{29 << 6, 0};
    ASSERT_TRUE(flash.program(kFaceTableOffset, &size, sizeof size));
    const uint32_t key = makeKey('A', 0, 0);
    writeRawRecord(flash, layout, 0, key, 9, 12);
    const Slot slot{key, kRecordHeaderBytes};
    ASSERT_TRUE(flash.program(layout.geometry.indexOffset + homeSlot(key, layout.geometry.indexSlots) * kSlotBytes,
                              &slot, sizeof slot));

    EXPECT_EQ(OpenResult::NotInstalled, store.open(kIdentityA)) << version;
    ASSERT_TRUE(installAndOpen(store, layout)) << version;  // claims faces 0-3 afresh: the old entry was erased
    EXPECT_GT(flash.eraseCalls(), 0u);
    EXPECT_EQ(Lookup::NotStored, store.find(key, nullptr)) << version;
    EXPECT_EQ(0u, store.logUsed());
    EXPECT_EQ(0u, store.usedSlots());
    EXPECT_EQ(0u, flash.reprograms());
  }
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
