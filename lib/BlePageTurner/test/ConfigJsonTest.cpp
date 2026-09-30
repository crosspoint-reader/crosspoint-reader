// The page turner's keys in settings.json. Moved from the app's BleRemoteSettingsTest: four
// remotes round-trip intact, garbage slots are dropped, a table past 8 slots is cut, a fifth
// remote is dropped, an empty table keeps its address; plus the keys of a file read off a
// device, loaded as they are.

#include <gtest/gtest.h>

#include "BlePageTurnerJson.h"

namespace bleturner {
namespace {

constexpr uint32_t kNextChapterTap = makeBinding(0x030102, false, Action::NextChapter);
constexpr uint32_t kPrevChapterHold = makeBinding(0x030008, true, Action::PrevChapter);

// settings.json from an X3 on v1.0.19 (28/09/2026), its page turner keys unchanged.
constexpr const char* kDeviceFile =
    R"({"sleepTimeoutMinutes":10,"blePageTurnerEnabled":1,"blePeerAddr":"7d:de:5c:bd:ae:ca",)"
    R"("blePeerName":"Free3-R","blePrevKeyUsage":1,"bleNextKeyUsage":2,"language":"VI"})";

TEST(BleConfigJsonTest, ADeviceFileLoadsAsItIsAndWritesBackTheSameKeys) {
  JsonDocument file;
  ASSERT_FALSE(deserializeJson(file, kDeviceFile));
  Config c;
  EXPECT_TRUE(readJson(c, file.as<JsonVariantConst>()));
  EXPECT_EQ(c.enabled, 1);
  EXPECT_STREQ(c.peerAddr, "7d:de:5c:bd:ae:ca");
  EXPECT_STREQ(c.peerName, "Free3-R");
  EXPECT_EQ(c.prevKeyUsage, 1);
  EXPECT_EQ(c.nextKeyUsage, 2);
  EXPECT_EQ(c.remoteCount, 0) << "no table saved: the built-in one for Free3 applies";

  JsonDocument out;
  writeJson(c, out);
  EXPECT_EQ(out["blePageTurnerEnabled"].as<int>(), 1);
  EXPECT_STREQ(out["blePeerAddr"].as<const char*>(), "7d:de:5c:bd:ae:ca");
  EXPECT_STREQ(out["blePeerName"].as<const char*>(), "Free3-R");
  EXPECT_EQ(out["blePrevKeyUsage"].as<int>(), 1);
  EXPECT_EQ(out["bleNextKeyUsage"].as<int>(), 2);
  EXPECT_TRUE(out["bleRemotes"].isNull()) << "no table, no key";
  EXPECT_EQ(out.size(), 5u);
}

TEST(BleConfigJsonTest, AFileFromBeforeThePageTurnerKeepsTheDefaultsAndAsksForASave) {
  JsonDocument file;
  ASSERT_FALSE(deserializeJson(file, R"({"sleepTimeoutMinutes":10})"));
  Config c;
  c.enabled = 1;
  c.remoteCount = 2;
  EXPECT_FALSE(readJson(c, file.as<JsonVariantConst>()));
  EXPECT_EQ(c.enabled, 0);
  EXPECT_STREQ(c.peerAddr, "");
  EXPECT_EQ(c.remoteCount, 0);
}

TEST(BleConfigJsonTest, FourRemotesRoundTripThroughTheSettingsFile) {
  Config c;
  const char* addrs[4] = {"11:22:33:44:55:01", "11:22:33:44:55:02", "11:22:33:44:55:03", "11:22:33:44:55:04"};
  for (int i = 0; i < 4; ++i) {
    RemoteTable* t = editableTable(c.remotes, c.remoteCount, addrs[i], "");
    ASSERT_NE(t, nullptr);
    if (i < 3) ASSERT_TRUE(learn(*t, Action::NextChapter, 0x030102 + i, false));
    if (i < 2) ASSERT_TRUE(learn(*t, Action::PrevChapter, 0x030008, true));
  }
  EXPECT_EQ(editableTable(c.remotes, c.remoteCount, "11:22:33:44:55:05", ""), nullptr) << "a fifth remote has no slot";

  JsonDocument saved;
  writeJson(c, saved);
  c.remoteCount = 0;
  ASSERT_TRUE(readJson(c, saved.as<JsonVariantConst>()));

  ASSERT_EQ(c.remoteCount, 4);
  for (int i = 0; i < 4; ++i) EXPECT_STREQ(c.remotes[i].addr, addrs[i]);
  EXPECT_EQ(c.remotes[0].count, 2);
  EXPECT_EQ(c.remotes[0].bindings[0], kNextChapterTap);
  EXPECT_EQ(c.remotes[0].bindings[1], kPrevChapterHold);
  EXPECT_EQ(c.remotes[2].count, 1);
  EXPECT_EQ(c.remotes[3].count, 0) << "a table cleared by hand keeps its address";
}

TEST(BleConfigJsonTest, GarbageSlotsAreDroppedAndOversizedTablesCut) {
  Config c;
  JsonDocument doc;
  JsonArray remotes = doc["bleRemotes"].to<JsonArray>();
  JsonObject a = remotes.add<JsonObject>();
  a["addr"] = "AA:BB:CC:DD:EE:01";
  JsonArray binds = a["binds"].to<JsonArray>();
  binds.add(kNextChapterTap);
  binds.add(0x70030102u);                                     // action out of range
  binds.add(0x18030102u);                                     // unknown bit
  binds.add("text");                                          // not a number
  binds.add(makeBinding(0x030100, false, Action::NextPage));  // zero value
  for (uint32_t i = 1; i <= 12; ++i) binds.add(makeBinding(0x020000 + i, false, Action::NextPage));
  JsonObject noAddr = remotes.add<JsonObject>();
  noAddr["binds"].to<JsonArray>().add(kNextChapterTap);
  for (int i = 0; i < 5; ++i) {
    JsonObject r = remotes.add<JsonObject>();
    r["addr"] = "AA:BB:CC:DD:EE:02";
    r["binds"].to<JsonArray>();
  }

  readJson(c, doc.as<JsonVariantConst>());
  ASSERT_EQ(c.remoteCount, kMaxRemotes);
  EXPECT_STREQ(c.remotes[0].addr, "AA:BB:CC:DD:EE:01");
  ASSERT_EQ(c.remotes[0].count, kMaxBindings);
  EXPECT_EQ(c.remotes[0].bindings[0], kNextChapterTap);
  for (uint8_t i = 0; i < c.remotes[0].count; ++i) EXPECT_TRUE(valid(c.remotes[0].bindings[i])) << i;
  EXPECT_STREQ(c.remotes[1].addr, "AA:BB:CC:DD:EE:02");

  // A file from before the tables has no key: no table, the key path.
  JsonDocument old;
  old["blePageTurnerEnabled"] = 1;
  ASSERT_TRUE(readJson(c, old.as<JsonVariantConst>()));
  EXPECT_EQ(c.remoteCount, 0);
  JsonDocument resaved;
  writeJson(c, resaved);
  EXPECT_TRUE(resaved["bleRemotes"].isNull()) << "no table, no key";
}

// The reader menu and save quotation actions go into the same 4-bit action field: a file
// written with them reads back with them, and a file from before them reads as it always did.
TEST(BleConfigJsonTest, ReaderShortcutActionsRoundTripAndOldSlotsStay) {
  Config c;
  const uint32_t menuTap = makeBinding(0x030001, false, Action::ReaderMenu);
  const uint32_t quoteHold = makeBinding(0x030008, true, Action::SaveQuote);
  JsonDocument doc;
  JsonObject r = doc["bleRemotes"].to<JsonArray>().add<JsonObject>();
  r["addr"] = "AA:BB:CC:DD:EE:07";
  JsonArray binds = r["binds"].to<JsonArray>();
  binds.add(kNextChapterTap);  // a slot written before the two actions existed
  binds.add(menuTap);
  binds.add(quoteHold);
  readJson(c, doc.as<JsonVariantConst>());
  ASSERT_EQ(c.remoteCount, 1);
  ASSERT_EQ(c.remotes[0].count, 3);
  EXPECT_EQ(c.remotes[0].bindings[0], kNextChapterTap);
  EXPECT_EQ(lookup(c.remotes[0], 0x030001, false), Action::ReaderMenu);
  EXPECT_EQ(lookup(c.remotes[0], 0x030008, true), Action::SaveQuote);

  JsonDocument resaved;
  writeJson(c, resaved);
  c.remoteCount = 0;
  readJson(c, resaved.as<JsonVariantConst>());
  ASSERT_EQ(c.remoteCount, 1);
  ASSERT_EQ(c.remotes[0].count, 3);
  EXPECT_EQ(c.remotes[0].bindings[1], menuTap);
  EXPECT_EQ(c.remotes[0].bindings[2], quoteHold);
  EXPECT_FALSE(valid(makeBinding(0x030001, false, static_cast<Action>(7))))
      << "an action past the list still reads as garbage";
}

}  // namespace
}  // namespace bleturner
