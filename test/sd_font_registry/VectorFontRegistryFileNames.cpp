#include <HalStorage.h>
#include <SdCardFontRegistry.h>
#include <gtest/gtest.h>

#include <string>

namespace {

class VectorFontRegistryFileNamesTest : public ::testing::Test {
 protected:
  void SetUp() override { fake::reset(); }
  void TearDown() override { fake::reset(); }
  SdCardFontRegistry registry;
};

TEST_F(VectorFontRegistryFileNamesTest, SupportedExtensionsAreCaseInsensitive) {
  static constexpr const char* styles[] = {"Regular", "Bold", "Italic", "BoldItalic"};
  for (const char* extension : {".ttf", ".TTF", ".otf", ".OtF", ".ttc", ".TtC"}) {
    SCOPED_TRACE(extension);
    fake::reset();
    for (const char* style : styles) {
      fake::add(std::string("/fonts/Family/Family-") + style + extension);
    }

    ASSERT_TRUE(registry.discover());
    const auto* family = registry.findFamily("Family");
    ASSERT_NE(family, nullptr);
    EXPECT_TRUE(family->vector);
    ASSERT_EQ(family->files.size(), 4u);
    for (uint8_t role = 0; role < 4; ++role) {
      const auto* file = family->findFile(0, role);
      ASSERT_NE(file, nullptr);
      EXPECT_EQ(std::string(file->name.get()), std::string("Family-") + styles[role] + extension);
    }
  }
}

TEST_F(VectorFontRegistryFileNamesTest, OneFamilyCanMixSupportedExtensions) {
  static constexpr const char* filenames[] = {"Family-Regular.otf", "Family-Bold.ttc", "Family-Italic.TTF",
                                              "Family-BoldItalic.OTF"};
  for (const char* filename : filenames) fake::add(std::string("/fonts/Family/") + filename);

  ASSERT_TRUE(registry.discover());
  const auto* family = registry.findFamily("Family");
  ASSERT_NE(family, nullptr);
  ASSERT_EQ(family->files.size(), 4u);
  for (uint8_t role = 0; role < 4; ++role) {
    const auto* file = family->findFile(0, role);
    ASSERT_NE(file, nullptr);
    EXPECT_STREQ(file->name.get(), filenames[role]);
  }
}

TEST_F(VectorFontRegistryFileNamesTest, UnsupportedExtensionsAndBackupSuffixesAreNotDiscovered) {
  for (const char* filename : {"Family-Regular.tf", "Family-Regular.ot", "Family-Regular.tc", "Family-Regular.ttff",
                               "Family-Regular.ttf.bak", "Family-Regular.otf.bak", "Family-Regular.ttc.bak",
                               "Family-Regular.ttf.tmp", "Family-Regular"}) {
    SCOPED_TRACE(filename);
    fake::reset();
    fake::add(std::string("/fonts/Family/") + filename);

    EXPECT_FALSE(registry.discover());
    EXPECT_EQ(registry.findFamily("Family"), nullptr);
  }
}

TEST_F(VectorFontRegistryFileNamesTest, DotAndUnderscorePrefixedFilesInsideFamiliesAreIgnored) {
  for (const char* root : {"/fonts", "/.fonts"}) {
    for (const char* extension : {".ttf", ".otf", ".ttc"}) {
      for (const char* prefix : {".", "_", "._"}) {
        SCOPED_TRACE(std::string(root) + "/" + prefix + "Hidden-Bold" + extension);
        fake::reset();
        const std::string directory = std::string(root) + "/Family/";
        const std::string visible = std::string("Visible-Regular") + extension;
        fake::add(directory + visible);
        fake::add(directory + prefix + "Hidden-Bold" + extension);

        ASSERT_TRUE(registry.discover());
        const auto* family = registry.findFamily("Family");
        ASSERT_NE(family, nullptr);
        ASSERT_EQ(family->files.size(), 1u);
        const auto* regular = family->findFile(0, 0);
        ASSERT_NE(regular, nullptr);
        EXPECT_EQ(std::string(regular->name.get()), visible);
        EXPECT_EQ(family->findFile(0, 1), nullptr);
      }
    }
  }
}

TEST_F(VectorFontRegistryFileNamesTest, DotAndUnderscorePrefixedLooseFilesAreIgnored) {
  for (const char* root : {"/fonts", "/.fonts"}) {
    for (const char* extension : {".ttf", ".otf", ".ttc"}) {
      for (const char* prefix : {".", "_", "._"}) {
        SCOPED_TRACE(std::string(root) + "/" + prefix + "Hidden" + extension);
        fake::reset();
        fake::add(std::string(root) + "/Visible.ttf");
        fake::add(std::string(root) + "/" + prefix + "Hidden" + extension);

        ASSERT_TRUE(registry.discover());
        EXPECT_EQ(registry.getFamilyCount(), 1);
        EXPECT_NE(registry.findFamily("Visible"), nullptr);
        EXPECT_EQ(registry.findFamily(std::string(prefix) + "Hidden"), nullptr);
      }
    }
  }
}

TEST_F(VectorFontRegistryFileNamesTest, DotAndUnderscorePrefixedFamilyDirectoriesAreIgnored) {
  for (const char* root : {"/fonts", "/.fonts"}) {
    for (const char* prefix : {".", "_", "._"}) {
      SCOPED_TRACE(std::string(root) + "/" + prefix + "Hidden");
      fake::reset();
      fake::add(std::string(root) + "/Visible/Visible-Regular.ttf");
      fake::add(std::string(root) + "/" + prefix + "Hidden/Hidden-Regular.ttf");

      ASSERT_TRUE(registry.discover());
      EXPECT_EQ(registry.getFamilyCount(), 1);
      EXPECT_NE(registry.findFamily("Visible"), nullptr);
      EXPECT_EQ(registry.findFamily(std::string(prefix) + "Hidden"), nullptr);
    }
  }
}

}  // namespace
