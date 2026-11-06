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
  EXPECT_EQ(static_cast<int>(Action::Confirm), 7);
  EXPECT_EQ(static_cast<int>(Action::Back), 8);
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
  EXPECT_FALSE(valid(0x90030102u)) << "action out of range";
  EXPECT_FALSE(valid(0x18030102u)) << "unknown bit";
  EXPECT_FALSE(valid(makeBinding(0x030100, false, Action::NextPage))) << "a zero value is a release, not a button";
}

TEST(BleBindingTableTest, AddressCopyIsBoundedAndTerminated) {
  for (const char* address : {"01", "01:02:03:04:05:06", "01:02:03:04:05:06extra"}) {
    RemoteTable tables[kMaxRemotes]{};
    uint8_t count = 0;
    const auto* table = editableTable(tables, count, address, "Remote");
    ASSERT_NE(table, nullptr);
    EXPECT_EQ(table->addr[sizeof(table->addr) - 1], '\0');
    EXPECT_STREQ(table->addr, strlen(address) < sizeof(table->addr) ? address : "01:02:03:04:05:06");
    EXPECT_EQ(count, 1);
  }
}

TEST(BleBindingTableTest, KeypageTakesBothCodeSetsOutOfTheBox) {
  // The same "BOOX Keypage" logged on the X3 on 06/10/2026 sent "04 00 00" from the upper
  // button and "08 00 00" from the lower one, report 3. The default takes both sets.
  const RemoteTable* t = tableFor(nullptr, 0, "AA:BB:CC:DD:EE:FF", "BOOX Keypage");
  ASSERT_TRUE(routes(t));
  EXPECT_EQ(lookup(*t, 0x030004, false), Action::NextPage);
  EXPECT_EQ(lookup(*t, 0x030008, false), Action::PrevPage);
  EXPECT_EQ(lookup(*t, 0x030002, false), Action::NextPage);
  EXPECT_EQ(lookup(*t, 0x030001, false), Action::PrevPage);
  EXPECT_EQ(lookup(*t, 0x030004, true), Action::None) << "a hold is not a tap";
  EXPECT_EQ(lookup(*t, 0x020004, false), Action::None) << "another report id is another button";

  // A table the user bound by hand wins over the default, whatever the remote sends.
  RemoteTable saved[1] = {};
  snprintf(saved[0].addr, sizeof saved[0].addr, "%s", "AA:BB:CC:DD:EE:FF");
  saved[0].count = 2;
  saved[0].bindings[0] = makeBinding(0x030002, false, Action::NextPage);
  saved[0].bindings[1] = makeBinding(0x030001, false, Action::PrevPage);
  const RemoteTable* own = tableFor(saved, 1, "AA:BB:CC:DD:EE:FF", "BOOX Keypage");
  ASSERT_EQ(own, &saved[0]);
  EXPECT_EQ(lookup(*own, 0x030004, false), Action::None);

  // Learning a direction on a table seeded from the default replaces both of its codes.
  RemoteTable tables[kMaxRemotes] = {};
  uint8_t count = 0;
  RemoteTable* edit = editableTable(tables, count, "AA:BB:CC:DD:EE:FF", "BOOX Keypage");
  ASSERT_NE(edit, nullptr);
  ASSERT_TRUE(learn(*edit, Action::NextPage, 0x030010, false));
  EXPECT_EQ(lookup(*edit, 0x030010, false), Action::NextPage);
  EXPECT_EQ(lookup(*edit, 0x030002, false), Action::None);
  EXPECT_EQ(lookup(*edit, 0x030004, false), Action::None);
  EXPECT_EQ(lookup(*edit, 0x030008, false), Action::PrevPage);

  // The other built-in default and unknown remotes are untouched.
  const auto* free3 = defaultTableFor("Free3-R");
  ASSERT_NE(free3, nullptr);
  EXPECT_EQ(free3->count, 2);
  EXPECT_EQ(lookup(*free3, 0x030004, false), Action::None);
  EXPECT_EQ(tableFor(nullptr, 0, "AA:BB:CC:DD:EE:FF", "Other"), nullptr);
}

TEST(BleBindingTableTest, MappingOneActionPreservesOtherButtonsAndRemotes) {
  RemoteTable tables[kMaxRemotes]{};
  uint8_t count = 0;
  auto* first = editableTable(tables, count, "01:02:03:04:05:06", "Free3");
  ASSERT_NE(first, nullptr);
  ASSERT_TRUE(learn(*first, Action::NextPage, 0x030001, false));
  ASSERT_TRUE(learn(*first, Action::PrevPage, 0x030002, false));
  auto* second = editableTable(tables, count, "11:12:13:14:15:16", "Remote");
  ASSERT_NE(second, nullptr);
  ASSERT_TRUE(learn(*second, Action::NextPage, 0x030001, false));

  ASSERT_TRUE(learn(*first, Action::NextChapter, 0x030004, false));
  EXPECT_EQ(lookup(*first, 0x030001, false), Action::NextPage);
  EXPECT_EQ(lookup(*first, 0x030002, false), Action::PrevPage);
  EXPECT_EQ(lookup(*first, 0x030004, false), Action::NextChapter);
  EXPECT_EQ(lookup(*first, 0x000008, false), Action::PrevChapter);

  ASSERT_TRUE(learn(*first, Action::PrevChapter, 0x030001, false));
  EXPECT_EQ(lookup(*first, 0x030001, false), Action::PrevChapter);
  Binding binding;
  EXPECT_FALSE(find(*first, Action::NextPage, binding));
  EXPECT_EQ(lookup(*second, 0x030001, false), Action::NextPage);
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
