// Button tables and the decoded key mapping, pure. The table tests moved from the app's
// ble_page_turner suite; the decode path through the SDK stays there.

#include <gtest/gtest.h>

#include "BlePageTurner.h"

namespace bleturner {
namespace {

// Stored in settings.json (bits 28-31 of a slot): a changed number misreads every saved table.
TEST(BleActionNumbersTest, ActionValuesNeverChange) {
  EXPECT_EQ(static_cast<int>(Action::None), 0);
  EXPECT_EQ(static_cast<int>(Action::NextPage), 1);
  EXPECT_EQ(static_cast<int>(Action::PrevPage), 2);
  EXPECT_EQ(static_cast<int>(Action::NextChapter), 3);
  EXPECT_EQ(static_cast<int>(Action::PrevChapter), 4);
  EXPECT_EQ(static_cast<int>(Action::ReaderMenu), 5);
  EXPECT_EQ(static_cast<int>(Action::SaveQuote), 6);
  EXPECT_EQ(makeBinding(0x030102, false, Action::NextChapter), 0x30030102u);
  EXPECT_EQ(makeBinding(0x030008, true, Action::PrevChapter), 0x41030008u);
}

TEST(BleBindingTableTest, KeyIdentityReadsAsTheKeyCode) {
  // A button known only by the key the decoder read (byte index 0xFF) is shown as
  // that key code, not as a byte position that does not exist.
  char text[16];
  formatCode(text, sizeof text, makeBinding(0xFFFF43, false, Action::NextPage));
  EXPECT_STREQ(text, "0x43");
  formatCode(text, sizeof text, makeBinding(0x030102, false, Action::NextPage));
  EXPECT_STREQ(text, "3:1=02");
}

TEST(BleBindingTableTest, FullTableRefusesAndGarbageIsNotValid) {
  RemoteTable t{};
  const Action actions[4] = {Action::NextPage, Action::PrevPage, Action::NextChapter, Action::PrevChapter};
  // Four actions, each learned on a new button: four slots, the old ones replaced.
  for (uint32_t round = 0; round < 3; ++round) {
    for (int i = 0; i < 4; ++i) EXPECT_TRUE(learn(t, actions[i], 0x030000 + round * 16 + i + 1, false));
  }
  EXPECT_EQ(t.count, 4);
  // The same button gesture for a second action moves it, never doubles it.
  EXPECT_TRUE(learn(t, Action::NextPage, 0x030000 + 32 + 2, false));
  EXPECT_EQ(lookup(t, 0x030000 + 32 + 2, false), Action::NextPage);
  EXPECT_EQ(t.count, 3);
  // A full table (eight slots read back from the card) says so instead of overwriting.
  t.count = kMaxBindings;
  for (uint8_t i = 0; i < t.count; ++i) {
    t.bindings[i] = makeBinding(0x010000 + i + 1, (i & 1) != 0, Action::NextPage);
  }
  EXPECT_FALSE(learn(t, Action::PrevPage, 0x7F0001, false));
  EXPECT_EQ(t.count, kMaxBindings);
  EXPECT_EQ(lookup(t, 0x7F0001, false), Action::None);

  EXPECT_TRUE(valid(makeBinding(0x030102, true, Action::PrevChapter)));
  EXPECT_FALSE(valid(makeBinding(0x030102, false, Action::None)));
  EXPECT_FALSE(valid(0x70030102u)) << "action out of range";
  EXPECT_FALSE(valid(0x18030102u)) << "unknown bit";
  EXPECT_FALSE(valid(makeBinding(0x030100, false, Action::NextPage))) << "a zero value is a release, not a button";
}

TEST(BlePageActionTest, DefaultKeysTurnUntilADirectionIsLearned) {
  Config c;
  c.enabled = 1;
  for (const uint8_t k : {kUsageLeft, kUsagePageUp, kUsageUp, kUsageBackspace, kUsageVolumeDown, kUsageScanPrev}) {
    EXPECT_EQ(pageActionFor(c, k, 0), Action::PrevPage) << static_cast<int>(k);
  }
  for (const uint8_t k :
       {kUsageRight, kUsagePageDown, kUsageDown, kUsageSpace, kUsageEnter, kUsageVolumeUp, kUsageScanNext}) {
    EXPECT_EQ(pageActionFor(c, k, 0), Action::NextPage) << static_cast<int>(k);
  }
  EXPECT_EQ(pageActionFor(c, 0x04, 0), Action::None) << "plain typing turns nothing";
  EXPECT_EQ(pageActionFor(c, kUsageRight, 0x02), Action::None) << "a key with a modifier turns nothing";
  EXPECT_EQ(pageActionFor(c, kUsageNone, 0), Action::None);

  c.nextKeyUsage = kUsageEnter;  // a remote whose Next button is Enter
  EXPECT_EQ(pageActionFor(c, kUsageEnter, 0), Action::NextPage);
  EXPECT_EQ(pageActionFor(c, kUsageRight, 0), Action::None) << "the learned key retires its direction's defaults";
  EXPECT_EQ(pageActionFor(c, kUsageLeft, 0), Action::PrevPage) << "the other direction keeps its defaults";

  c.enabled = 0;
  EXPECT_EQ(pageActionFor(c, kUsageEnter, 0), Action::None);
}

}  // namespace
}  // namespace bleturner
