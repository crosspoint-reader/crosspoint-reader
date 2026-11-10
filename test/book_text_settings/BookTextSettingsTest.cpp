#include <gtest/gtest.h>

#include <cstring>

#include "BookTextSettings.h"
#include "CrossPointSettings.h"
#include "HalStorage.h"

class BookTextSettingsTest : public testing::Test {
 protected:
  void SetUp() override {
    Storage = HalStorage{};
    textSettingsSaveFails = false;
    ReaderTextSettings{}.saveDefaults();
    textSettingsDefaultWrites = 0;
  }
};

TEST_F(BookTextSettingsTest, OnlyChangedFieldsArePinnedAndDefaultsRemainLive) {
  BookTextSettings book;
  book.load("/a");
  auto edited = book.resolve();
  edited.fontPointSize = 18;
  ASSERT_TRUE(book.update(edited));
  EXPECT_EQ(Storage.files["/a/text-settings.bin"][1], 2);
  SETTINGS.screenMargin = 20;
  SETTINGS.fontPointSize = 16;
  EXPECT_EQ(book.resolve().fontPointSize, 18);
  EXPECT_EQ(book.resolve().screenMargin, 20);
  EXPECT_EQ(textSettingsDefaultWrites, 0U);
}

TEST_F(BookTextSettingsTest, NoOpDoesNotWriteOrRemoveAnOverrideThatNowEqualsDefault) {
  BookTextSettings book;
  book.load("/a");
  ASSERT_TRUE(book.update(book.resolve()));
  EXPECT_EQ(Storage.writes, 0U);
  EXPECT_FALSE(book.hasOverrides());
  auto edited = book.resolve();
  edited.fontPointSize = 18;
  ASSERT_TRUE(book.update(edited));
  SETTINGS.fontPointSize = 18;
  ASSERT_TRUE(book.update(book.resolve()));
  EXPECT_EQ(Storage.writes, 1U);
  SETTINGS.fontPointSize = 16;
  EXPECT_EQ(book.resolve().fontPointSize, 18);
}

TEST_F(BookTextSettingsTest, EqualDefaultEditAndResetRemoveTheRecord) {
  BookTextSettings book;
  book.load("/a");
  auto edited = book.resolve();
  edited.fontPointSize = 18;
  ASSERT_TRUE(book.update(edited));
  ASSERT_TRUE(book.update(ReaderTextSettings::defaults()));
  EXPECT_FALSE(book.hasOverrides());
  EXPECT_FALSE(Storage.exists("/a/text-settings.bin"));
  ASSERT_TRUE(book.update(edited));
  ASSERT_TRUE(book.reset());
  EXPECT_EQ(book.resolve(), ReaderTextSettings::defaults());
  EXPECT_FALSE(Storage.exists("/a/text-settings.bin"));
}

TEST_F(BookTextSettingsTest, AllFieldsRoundTripWithoutLeakingBetweenBooks) {
  BookTextSettings book;
  book.load("/a");
  ReaderTextSettings edited;
  std::strcpy(edited.sdFontFamilyName, "Noto Serif SC");
  edited.fontPointSize = 22;
  edited.lineSpacing = 5;
  edited.wordSpacing = 150;
  edited.characterSpacing = 4;
  edited.extraParagraphSpacing = 0;
  edited.paragraphIndentSpaces = 5;
  edited.paragraphAlignment = 4;
  edited.screenMargin = 25;
  edited.focusReadingEnabled = 1;
  edited.hyphenationEnabled = 1;
  edited.embeddedStyle = 0;
  edited.textAntiAliasing = 0;
  ASSERT_TRUE(book.update(edited));
  EXPECT_EQ(Storage.files["/a/text-settings.bin"].size(), 47U);
  BookTextSettings restored;
  restored.load("/a");
  EXPECT_EQ(restored.resolve(), edited);
  restored.load("/b");
  EXPECT_EQ(restored.resolve(), ReaderTextSettings::defaults());
}

TEST_F(BookTextSettingsTest, InvalidRecordsUseDefaultsWithoutRewriting) {
  BookTextSettings book;
  book.load("/a");
  auto edited = book.resolve();
  edited.fontPointSize = 18;
  ASSERT_TRUE(book.update(edited));
  const auto valid = Storage.files["/a/text-settings.bin"];
  for (int corruption = 0; corruption < 5; ++corruption) {
    auto bytes = valid;
    switch (corruption) {
      case 0:
        bytes.pop_back();
        break;
      case 1:
        bytes[0] = 99;
        break;
      case 2:
        bytes[2] = 128;
        break;
      case 3:
        bytes[35] = 0;
        break;
      case 4:
        bytes[1] = 1;
        std::memset(bytes.data() + 3, 'x', 32);
        break;
    }
    Storage.files["/a/text-settings.bin"] = bytes;
    book.load("/a");
    EXPECT_EQ(book.resolve(), ReaderTextSettings::defaults());
    EXPECT_EQ(Storage.files["/a/text-settings.bin"], bytes);
  }
  EXPECT_EQ(Storage.writes, 1U);
}

TEST_F(BookTextSettingsTest, InactiveFieldsAreIgnored) {
  auto& bytes = Storage.files["/a/text-settings.bin"];
  bytes.assign(47, 255);
  bytes[0] = 1;
  bytes[1] = 2;
  bytes[2] = 0;
  bytes[35] = 18;
  BookTextSettings book;
  book.load("/a");
  auto expected = ReaderTextSettings::defaults();
  expected.fontPointSize = 18;
  EXPECT_EQ(book.resolve(), expected);
}

TEST_F(BookTextSettingsTest, FailedSaveAndResetKeepPriorSelection) {
  BookTextSettings book;
  book.load("/a");
  auto edited = book.resolve();
  edited.fontPointSize = 18;
  ASSERT_TRUE(book.update(edited));
  const auto previousBytes = Storage.files["/a/text-settings.bin"];
  auto next = edited;
  next.screenMargin = 20;
  Storage.failWrite = true;
  EXPECT_FALSE(book.update(next));
  Storage.failWrite = false;
  Storage.shortWrite = true;
  EXPECT_FALSE(book.update(next));
  EXPECT_EQ(book.resolve(), edited);
  EXPECT_EQ(Storage.files["/a/text-settings.bin"], previousBytes);
  Storage.failRemove = true;
  EXPECT_FALSE(book.reset());
  EXPECT_EQ(book.resolve(), edited);
}

TEST_F(BookTextSettingsTest, DefaultSaveFailureRestoresValuesAndSpecUsesTheRequest) {
  auto edited = ReaderTextSettings::defaults();
  edited.fontPointSize = 18;
  textSettingsSaveFails = true;
  EXPECT_FALSE(edited.saveDefaults());
  EXPECT_EQ(ReaderTextSettings::defaults(), ReaderTextSettings{});
  edited.characterSpacing = 4;
  edited.paragraphIndentSpaces = 5;
  edited.lineSpacing = 4;
  const auto spec = edited.renderSpec(42, 400, 700, CrossPointSettings::IMAGES_DISPLAY_BW);
  EXPECT_EQ(spec.fontId, 42);
  EXPECT_EQ(spec.characterSpacing, 2);
  EXPECT_EQ(spec.paragraphIndentSpaces, 5);
  EXPECT_FLOAT_EQ(spec.lineCompression, 1.45f);
  EXPECT_EQ(spec.imageRendering, CrossPointSettings::IMAGES_DISPLAY);
}
