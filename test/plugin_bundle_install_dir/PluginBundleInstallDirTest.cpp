#include <HalStorage.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "util/PluginLocations.h"

namespace {
class BundleDirTest : public ::testing::Test {
 protected:
  void SetUp() override {
    card = std::filesystem::temp_directory_path() /
           ("cp-bundle-dir-" + std::to_string(static_cast<long long>(
                                   std::filesystem::file_time_type::clock::now().time_since_epoch().count())));
    std::filesystem::create_directories(card);
    Storage.card = card;
  }
  void TearDown() override { std::filesystem::remove_all(card); }
  void makeDir(const std::string& path) { std::filesystem::create_directories(card / path.substr(1)); }
  void makeFile(const std::string& path, const std::string& data) {
    const auto file = card / path.substr(1);
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file) << data;
  }
  std::filesystem::path card;
};
}  // namespace

TEST_F(BundleDirTest, UpdatesExistingPluginInItsDiscoverableRoot) {
  makeDir("/plugins/foo");
  makeFile("/plugins/foo/token.json", "grant");
  makeFile("/plugins/foo/config.json", "settings");
  makeFile("/books/keep.epub", "book");
  EXPECT_EQ(PluginLocations::findPluginDir("foo"), "/plugins/foo");
  EXPECT_EQ(PluginLocations::bundleInstallDir("/.crosspoint/plugins", "foo"), "/plugins/foo");
  EXPECT_EQ(PluginLocations::bundleInstallDir("/.crosspoint/plugins/", "foo"), "/plugins/foo");
  EXPECT_FALSE(std::filesystem::exists(card / ".crosspoint/plugins/foo"));
  EXPECT_EQ(std::filesystem::file_size(card / "plugins/foo/token.json"), 5);
  EXPECT_EQ(std::filesystem::file_size(card / "plugins/foo/config.json"), 8);
  EXPECT_EQ(std::filesystem::file_size(card / "books/keep.epub"), 4);
}

TEST_F(BundleDirTest, FreshInstallAndConfigOverrideKeepSelectedPluginRoot) {
  EXPECT_EQ(PluginLocations::bundleInstallDir("/.crosspoint/plugins", "foo"), "/.crosspoint/plugins/foo");
  EXPECT_EQ(PluginLocations::bundleInstallDir("/plugins", "foo"), "/plugins/foo");
  EXPECT_EQ(PluginLocations::bundleInstallDir("/.plugins/", "foo"), "/.plugins/foo");
  makeDir("/.plugins/foo");
  EXPECT_EQ(PluginLocations::bundleInstallDir("/plugins", "foo"), "/.plugins/foo");
}

TEST_F(BundleDirTest, ExistingRootPriorityIncludingEmptyFolderIsPreserved) {
  makeDir("/.plugins/foo");
  makeDir("/plugins/foo");
  makeDir("/.crosspoint/plugins/foo");
  EXPECT_EQ(PluginLocations::bundleInstallDir("/.plugins", "foo"), "/.crosspoint/plugins/foo");
  std::filesystem::remove_all(card / ".crosspoint/plugins/foo");
  EXPECT_EQ(PluginLocations::bundleInstallDir("/.crosspoint/plugins", "foo"), "/plugins/foo");
}

TEST_F(BundleDirTest, GenericAndNonSingleFolderBundlesKeepTheirDestination) {
  makeDir("/plugins/foo");
  EXPECT_EQ(PluginLocations::bundleInstallDir("/themes", "foo"), "/themes/foo");
  EXPECT_EQ(PluginLocations::bundleInstallDir("/books", "foo"), "/books/foo");
  EXPECT_EQ(PluginLocations::bundleInstallDir("/", "foo"), "/foo");
  EXPECT_EQ(PluginLocations::bundleInstallDir("/.crosspoint/plugins", "group/foo"), "/.crosspoint/plugins/group/foo");
  EXPECT_EQ(PluginLocations::bundleInstallDir("/.crosspoint/plugins", ".hidden"), "/.crosspoint/plugins/.hidden");
}

TEST_F(BundleDirTest, RegularFileCollisionIsReturnedForCallerToReject) {
  makeFile("/.crosspoint/plugins/foo", "file");
  makeDir("/plugins/foo");
  const auto resolved = PluginLocations::bundleInstallDir("/.crosspoint/plugins", "foo");
  EXPECT_EQ(resolved, "/.crosspoint/plugins/foo");
  EXPECT_FALSE(Storage.open(resolved.c_str()).isDirectory());
}
