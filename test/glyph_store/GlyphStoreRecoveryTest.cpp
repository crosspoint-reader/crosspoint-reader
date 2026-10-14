// Where writing resumes after a power cut (spec 4.5): the log scan never lets a byte be programmed twice.
#include <gtest/gtest.h>

#include <vector>

#include "GlyphStoreTestUtil.h"

using namespace glyphstore;
using namespace gst_test;
using OpenResult = FlashGlyphStore::OpenResult;

namespace {

class GlyphStoreRecovery : public StoreTest {
 protected:
  void SetUp() override { ASSERT_TRUE(install(store, layout)); }

  // Opens from scratch, as after a reboot.
  void reopen() {
    store.close();
    ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  }

  // Two complete records at the start of the log; returns where the next one would go.
  uint32_t writeTwo() {
    writeRawRecord(flash, layout, 0, makeKey('a', 0, 0), 9, 12);
    const uint32_t second = recordSize(9, 12);
    writeRawRecord(flash, layout, second, makeKey('b', 0, 0), 10, 12);
    return second + recordSize(10, 12);
  }

  const Layout layout = smallLayout();
};

}  // namespace

TEST_F(GlyphStoreRecovery, WritingResumesAfterTheLastRecord) {
  const uint32_t end = writeTwo();
  reopen();
  EXPECT_EQ(end, store.logUsed());
}

TEST_F(GlyphStoreRecovery, ATornHeaderMovesTheResumePointToTheNextErasedSector) {
  const uint32_t end = writeTwo();
  const uint8_t garbage[10] = {0x12, 0x00, 0x00, 0x08, 0x33};  // key 0x08000012: bit 27 set, not a record
  ASSERT_TRUE(flash.program(layout.geometry.logOffset + end, garbage, sizeof garbage));
  reopen();
  EXPECT_EQ(kSectorBytes, store.logUsed());
}

TEST_F(GlyphStoreRecovery, BytesBehindAnErasedKeyAreNotOverwritten) {
  // A cut can leave a record's later bytes programmed while its key still reads erased.
  const uint32_t end = writeTwo();
  const uint8_t tail[6] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05};
  ASSERT_TRUE(flash.program(layout.geometry.logOffset + end + 8, tail, sizeof tail));
  reopen();
  EXPECT_EQ(kSectorBytes, store.logUsed());
}

TEST_F(GlyphStoreRecovery, ARecordWithACompleteHeaderButAShortBitmapIsSkippedWhole) {
  const uint32_t end = writeTwo();
  writeRawRecord(flash, layout, end, makeKey('c', 0, 0), 20, 20, 30);  // 30 of its 100 bitmap bytes
  reopen();
  EXPECT_EQ(end + recordSize(20, 20), store.logUsed());
}

TEST_F(GlyphStoreRecovery, AGarbledPageAfterTheLastRecordIsSkipped) {
  // A real chip cut mid-page can leave a whole 256-byte page partly programmed.
  writeTwo();
  std::vector<uint8_t> page(256);
  for (uint32_t i = 0; i < page.size(); ++i) page[i] = static_cast<uint8_t>(0xFF ^ (i * 37));
  ASSERT_TRUE(flash.program(layout.geometry.logOffset + 4096 + 100, page.data(), 256));
  reopen();
  EXPECT_EQ(2 * kSectorBytes, store.logUsed());
}

TEST(GlyphStoreRecoveryEdge, GarbageAtTheVeryEndLeavesTheLogFull) {
  SimNorFlash flash(kFlashBytes);
  FlashGlyphStore store(flash);
  const Layout layout = smallLayout(1024, 512, 8192);
  ASSERT_TRUE(install(store, layout));
  writeRawRecord(flash, layout, 0, makeKey('a', 0, 0), 9, 12);
  const uint8_t junk = 0x00;
  ASSERT_TRUE(flash.program(layout.geometry.logOffset + 8190, &junk, 1));
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  EXPECT_EQ(8192u, store.logUsed());
  EXPECT_EQ(0u, flash.violations());
}
