#include <HalStorage.h>
#include <SdCardFontRegistry.h>
#include <gtest/gtest.h>

TEST(FontRegistry, RestoresBackupOnlyFontsInBothRoots) {
  for (const char* root : {"/.fonts", "/fonts"}) {
    fake::reset();
    const std::string destination = std::string(root) + "/Example/Example_14.cpfont";
    fake::add(destination + ".bak", "old font");
    SdCardFontRegistry registry;
    ASSERT_TRUE(registry.discover());
    ASSERT_TRUE(Storage.exists(destination.c_str()));
    EXPECT_FALSE(Storage.exists((destination + ".bak").c_str()));
    const auto* family = registry.findFamily("Example");
    ASSERT_NE(family, nullptr);
    ASSERT_NE(family->findFile(14), nullptr);
    EXPECT_EQ(family->findFile(14)->path, destination);
  }
}

TEST(FontRegistry, ExistingFontWinsOverBackup) {
  fake::reset();
  const std::string destination = "/fonts/Example/Example_14.cpfont";
  fake::add(destination, "new font");
  fake::add(destination + ".bak", "old font");
  SdCardFontRegistry registry;
  ASSERT_TRUE(registry.discover());
  const auto* family = registry.findFamily("Example");
  ASSERT_NE(family, nullptr);
  EXPECT_EQ(family->files.size(), 1);
  EXPECT_EQ(std::string(fake::files.at(destination)->bytes.begin(), fake::files.at(destination)->bytes.end()),
            "new font");
}

TEST(FontRegistry, BackupNamesIncludeTheLongestAcceptedBitmapFilename) {
  for (const char* root : {"/.fonts", "/fonts"}) {
    for (size_t baseLength : {117U, 120U, 127U, 128U}) {
      fake::reset();
      const std::string name = std::string(baseLength - 3, 'A') + "_14.cpfont";
      const std::string destination = std::string(root) + "/Example/" + name;
      fake::add(destination + ".bak", "old font");
      SdCardFontRegistry registry;
      const bool accepted = baseLength <= 127;
      EXPECT_EQ(registry.discover(), accepted) << name.size();
      EXPECT_EQ(Storage.exists(destination.c_str()), accepted);
      EXPECT_EQ(Storage.exists((destination + ".bak").c_str()), !accepted);
      if (accepted) {
        const auto* family = registry.findFamily("Example");
        ASSERT_NE(family, nullptr);
        ASSERT_NE(family->findFile(14), nullptr);
        EXPECT_EQ(family->findFile(14)->path, destination);
      }
    }
  }
}

TEST(FontRegistry, FailedRecoveryKeepsBackupForNextDiscovery) {
  fake::reset();
  const std::string destination = "/fonts/Example/Example_14.cpfont";
  fake::add(destination + ".bak", "old font");
  fake::renameFailures[destination + ".bak"] = 1;
  SdCardFontRegistry registry;
  EXPECT_FALSE(registry.discover());
  ASSERT_TRUE(Storage.exists((destination + ".bak").c_str()));
  EXPECT_TRUE(registry.discover());
  EXPECT_TRUE(Storage.exists(destination.c_str()));
}

TEST(FontRegistry, UnrecognizedBackupIsIgnored) {
  fake::reset();
  fake::add("/fonts/Example/notes.txt.bak");
  SdCardFontRegistry registry;
  EXPECT_FALSE(registry.discover());
  EXPECT_TRUE(Storage.exists("/fonts/Example/notes.txt.bak"));
  EXPECT_FALSE(Storage.exists("/fonts/Example/notes.txt"));
}

#if CROSSPOINT_VECTOR_FONTS
TEST(FontRegistry, RestoresVectorFontBackup) {
  fake::reset();
  fake::add("/fonts/Example/Example.ttf.bak", "old font");
  SdCardFontRegistry registry;
  ASSERT_TRUE(registry.discover());
  const auto* family = registry.findFamily("Example");
  ASSERT_NE(family, nullptr);
  ASSERT_TRUE(family->vector);
  ASSERT_EQ(family->files.size(), 1);
  EXPECT_EQ(family->files[0].path, "/fonts/Example/Example.ttf");
}

TEST(FontRegistry, RestoresLooseVectorBackupsInBothRoots) {
  for (const char* root : {"/.fonts", "/fonts"}) {
    for (const char* extension : {".ttf", ".OTF", ".ttc"}) {
      for (bool loose : {false, true}) {
        for (size_t baseLength : {8U, 123U, 127U, 128U}) {
          fake::reset();
          const std::string name(baseLength, 'A');
          const std::string destination = std::string(root) + (loose ? "/" : "/Example/") + name + extension;
          fake::add(destination + ".bak", "old font");
          SdCardFontRegistry registry;
          const bool accepted = baseLength <= 127;
          EXPECT_EQ(registry.discover(), accepted) << destination;
          EXPECT_EQ(Storage.exists(destination.c_str()), accepted);
          EXPECT_EQ(Storage.exists((destination + ".bak").c_str()), !accepted);
          if (accepted) {
            const auto* family = registry.findFamily(loose ? name : "Example");
            ASSERT_NE(family, nullptr);
            ASSERT_EQ(family->files.size(), 1);
            EXPECT_EQ(family->files[0].path, destination);
          }
        }
      }
    }
  }
}

TEST(FontRegistry, LooseVectorRecoveryPreservesExistingAndFailedBackups) {
  for (const char* root : {"/.fonts", "/fonts"}) {
    const std::string destination = std::string(root) + "/Bookerly.ttf";
    fake::reset();
    fake::add(destination, "new font");
    fake::add(destination + ".bak", "old font");
    SdCardFontRegistry registry;
    ASSERT_TRUE(registry.discover());
    EXPECT_EQ(std::string(fake::files.at(destination)->bytes.begin(), fake::files.at(destination)->bytes.end()),
              "new font");
    EXPECT_TRUE(Storage.exists((destination + ".bak").c_str()));
    Storage.remove(destination.c_str());
    fake::renameFailures[destination + ".bak"] = 1;
    EXPECT_FALSE(registry.discover());
    EXPECT_TRUE(Storage.exists((destination + ".bak").c_str()));
    EXPECT_TRUE(registry.discover());
    EXPECT_TRUE(Storage.exists(destination.c_str()));
  }
}
#else
TEST(FontRegistry, VectorBackupsRemainIgnoredWithoutVectorSupport) {
  fake::reset();
  fake::add("/fonts/Bookerly.ttf.bak", "old font");
  SdCardFontRegistry registry;
  EXPECT_FALSE(registry.discover());
  EXPECT_TRUE(Storage.exists("/fonts/Bookerly.ttf.bak"));
  EXPECT_FALSE(Storage.exists("/fonts/Bookerly.ttf"));
}
#endif
