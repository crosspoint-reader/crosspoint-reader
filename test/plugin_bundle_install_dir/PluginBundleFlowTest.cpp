#include <HalStorage.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "network/ProtectedPaths.h"
#include "util/PluginLocations.h"

#define LOG_ERR(...) ((void)0)

struct ScopedCleanup {
  std::function<void()> action;
  ~ScopedCleanup() { action(); }
};

class HttpDownloader {
 public:
  enum DownloadError { OK, HTTP_ERROR, FILE_ERROR, ABORTED };
};

namespace pluginevents {
void refreshSubscriptions() {}
}  // namespace pluginevents

void emitBookDownloaded(const std::string&, const std::string&, const std::string&) {}

class PluginCatalogActivity {
 public:
  struct Item {
    std::string id, title, base;
    std::vector<std::string> files;
  };
  struct Manifest {
    std::string bundleSubdir = "{id}";
  } manifest;
  std::unique_ptr<int> session;
  std::string manifestPath = "/plugins/hub/device.json";
  std::string targetRoot = "/.crosspoint/plugins";
  int failAt = -1;
  HttpDownloader::DownloadError failure = HttpDownloader::HTTP_ERROR;
  int fetched = 0;
  int openHandlesDuringFetch = 0;

  HttpDownloader::DownloadError downloadBundle(const Item& item);
  std::string substituted(std::string text, const Item* item) const {
    const auto pos = text.find("{id}");
    if (pos != std::string::npos) text.replace(pos, 4, item->id);
    return text;
  }
  std::string downloadDir() const { return targetRoot; }
  HttpDownloader::DownloadError downloadFile(const std::string&, const std::string& destination) {
    openHandlesDuringFetch = std::max(openHandlesDuringFetch, HalFile::activeHandles);
    if (fetched++ == failAt) return failure;
    const auto file = Storage.card / destination.substr(1);
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file) << "new:" << file.stem().string();
    return HttpDownloader::OK;
  }
};

#include "DownloadBundleBody.inc"

namespace {
class BundleFlowTest : public ::testing::Test {
 protected:
  void SetUp() override {
    card = std::filesystem::temp_directory_path() /
           ("cp-bundle-flow-" + std::to_string(static_cast<long long>(
                                    std::filesystem::file_time_type::clock::now().time_since_epoch().count())));
    std::filesystem::create_directories(card);
    Storage.card = card;
    write("/plugins/foo/device.json", "old manifest");
    write("/plugins/foo/token.json", "grant");
    write("/plugins/foo/config.json", "settings");
    write("/books/book.epub", "book");
    write("/.crosspoint/settings.json", "reader settings");
    item.id = "foo";
    item.title = "Foo";
    item.base = "https://example.test/foo";
    item.files = {"device.json", "README.md"};
  }
  void TearDown() override { std::filesystem::remove_all(card); }
  void write(const std::string& path, const std::string& data) {
    const auto file = card / path.substr(1);
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file) << data;
  }
  std::string read(const std::string& path) const {
    std::ifstream file(card / path.substr(1));
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  }
  void expectPrivateFiles() const {
    EXPECT_EQ(read("/plugins/foo/token.json"), "grant");
    EXPECT_EQ(read("/plugins/foo/config.json"), "settings");
    EXPECT_EQ(read("/books/book.epub"), "book");
    EXPECT_EQ(read("/.crosspoint/settings.json"), "reader settings");
    EXPECT_FALSE(std::filesystem::exists(card / ".crosspoint/plugins/foo"));
  }
  std::filesystem::path card;
  PluginCatalogActivity activity;
  PluginCatalogActivity::Item item;
};
}  // namespace

TEST_F(BundleFlowTest, SuccessfulUpdateKeepsGrantConfigBooksAndSettings) {
  EXPECT_EQ(activity.downloadBundle(item), HttpDownloader::OK);
  EXPECT_EQ(activity.openHandlesDuringFetch, 0);
  EXPECT_EQ(PluginLocations::findPluginDir("foo"), "/plugins/foo");
  EXPECT_EQ(read("/plugins/foo/device.json"), "new:device.json");
  EXPECT_EQ(read("/plugins/foo/README.md"), "new:README.md");
  expectPrivateFiles();
}

TEST_F(BundleFlowTest, CancelledFetchLeavesInstalledFiles) {
  activity.failAt = 1;
  activity.failure = HttpDownloader::ABORTED;
  EXPECT_EQ(activity.downloadBundle(item), HttpDownloader::ABORTED);
  EXPECT_EQ(read("/plugins/foo/device.json"), "old manifest");
  EXPECT_FALSE(std::filesystem::exists(card / "plugins/foo/device.json.new"));
  expectPrivateFiles();
}

TEST_F(BundleFlowTest, FailedFetchLeavesInstalledFiles) {
  activity.failAt = 1;
  EXPECT_EQ(activity.downloadBundle(item), HttpDownloader::HTTP_ERROR);
  EXPECT_EQ(read("/plugins/foo/device.json"), "old manifest");
  EXPECT_FALSE(std::filesystem::exists(card / "plugins/foo/device.json.new"));
  expectPrivateFiles();
}

TEST_F(BundleFlowTest, ExistingRegularFileBlocksInstall) {
  std::filesystem::remove_all(card / "plugins/foo");
  write("/.crosspoint/plugins/foo", "file");
  EXPECT_EQ(activity.downloadBundle(item), HttpDownloader::FILE_ERROR);
  EXPECT_EQ(read("/.crosspoint/plugins/foo"), "file");
}

TEST_F(BundleFlowTest, EarlierEmptyFolderKeepsDiscoveryPriority) {
  std::filesystem::create_directories(card / ".crosspoint/plugins/foo");
  EXPECT_EQ(PluginLocations::findPluginDir("foo"), "/.crosspoint/plugins/foo");
  EXPECT_EQ(activity.downloadBundle(item), HttpDownloader::OK);
  EXPECT_EQ(read("/.crosspoint/plugins/foo/device.json"), "new:device.json");
  EXPECT_EQ(read("/plugins/foo/token.json"), "grant");
}

TEST_F(BundleFlowTest, GenericAndNestedBundlesKeepTheirConfiguredDestination) {
  activity.targetRoot = "/themes";
  EXPECT_EQ(activity.downloadBundle(item), HttpDownloader::OK);
  EXPECT_EQ(read("/themes/foo/device.json"), "new:device.json");
  EXPECT_EQ(read("/plugins/foo/device.json"), "old manifest");
  activity.targetRoot = "/.crosspoint/plugins";
  activity.manifest.bundleSubdir = "collection/{id}";
  EXPECT_EQ(activity.downloadBundle(item), HttpDownloader::OK);
  EXPECT_EQ(read("/.crosspoint/plugins/collection/foo/device.json"), "new:device.json");
  EXPECT_EQ(read("/plugins/foo/device.json"), "old manifest");
}

TEST_F(BundleFlowTest, InvalidSubdirectoriesDoNotWrite) {
  for (const std::string& bad : {"", ".", "..", "/foo", "foo\\bar"}) {
    activity.manifest.bundleSubdir = bad;
    EXPECT_EQ(activity.downloadBundle(item), HttpDownloader::FILE_ERROR) << bad;
    EXPECT_EQ(read("/plugins/foo/device.json"), "old manifest");
    EXPECT_FALSE(std::filesystem::exists(card / ".crosspoint/plugins/foo"));
  }
}
