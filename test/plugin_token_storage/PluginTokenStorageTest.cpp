#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "network/ProtectedPaths.h"
#include "util/PluginHttp.h"

#define LOG_ERR(...) ((void)0)

namespace {
struct PowerCut {};
struct StorageModel {
  std::map<std::string, std::string> files;
  std::map<std::string, int> handles;
  std::string failOpen, failRead, failRemove, failRenameFrom, failRenameTo;
  bool failClose = false;
  bool shortWrite = false;
  bool corruptWrite = false;
  int cutAfter = -1;
  int mutations = 0;

  void changed() {
    if (++mutations == cutAfter) throw PowerCut{};
  }
  bool exists(const char* path) const { return files.contains(path); }
  bool remove(const char* path) {
    if (failRemove == path) return false;
    EXPECT_EQ(handles[path], 0);
    const bool removed = files.erase(path) != 0;
    if (removed) changed();
    return removed;
  }
  bool rename(const char* from, const char* to) {
    if (failRenameFrom == from || failRenameTo == to || !files.contains(from) || files.contains(to)) return false;
    EXPECT_EQ(handles[from], 0);
    EXPECT_EQ(handles[to], 0);
    files[to] = files.at(from);
    files.erase(from);
    changed();
    return true;
  }
  bool readFileToString(const char*, const std::string& path, size_t cap, std::string& out) {
    if (failRead == path || !files.contains(path) || files.at(path).size() > cap) return false;
    out = files.at(path);
    return true;
  }
};
StorageModel model;
}  // namespace

struct HalFile {
  std::string path;
  ~HalFile() { close(); }
  HalFile() = default;
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;
  bool isOpen() const { return !path.empty(); }
  bool close() {
    if (path.empty()) return true;
    --model.handles[path];
    path.clear();
    return !model.failClose;
  }
  void flush() { model.changed(); }
  size_t write(const char* data, size_t bytes) {
    const size_t written = model.shortWrite ? bytes / 2 : bytes;
    model.files[path].append(data, written);
    if (model.corruptWrite && written) model.files[path][0] = '!';
    model.changed();
    return written;
  }
};

struct StorageAdapter {
  bool openFileForWrite(const char*, const std::string& path, HalFile& file) {
    if (model.failOpen == path) return false;
    file.path = path;
    ++model.handles[path];
    model.files[path].clear();
    model.changed();
    return true;
  }
  bool exists(const char* path) const { return model.exists(path); }
  bool remove(const char* path) { return model.remove(path); }
  bool rename(const char* from, const char* to) { return model.rename(from, to); }
  bool readFileToString(const char* module, const std::string& path, size_t cap, std::string& out) {
    return model.readFileToString(module, path, cap, out);
  }
} Storage;

#include "TokenFunctions.inc"

namespace {
constexpr const char* TOKEN = "/plugins/test/token.json";
const std::string OLD = R"({"auth":{"token":"old"}})";

class PluginTokenStorage : public ::testing::Test {
 protected:
  void SetUp() override {
    model = {};
    model.files[TOKEN] = OLD;
    model.files["/plugins/test/config.json"] = "private settings";
  }
  bool save(const std::string& value = "new") { return pluginhttp::saveTokenToFile(TOKEN, "auth.token", value); }
  std::string load() {
    std::string token;
    EXPECT_TRUE(pluginhttp::loadTokenFromFile(TOKEN, "auth.token", token));
    return token;
  }
  void TearDown() override {
    for (const auto& [path, count] : model.handles) EXPECT_EQ(count, 0) << path;
    EXPECT_EQ(model.files["/plugins/test/config.json"], "private settings");
  }
};

TEST_F(PluginTokenStorage, ShortWritePreservesCurrent) {
  model.shortWrite = true;
  EXPECT_FALSE(save());
  EXPECT_EQ(load(), "old");
}

TEST_F(PluginTokenStorage, FailedClosePreservesCurrent) {
  model.failClose = true;
  EXPECT_FALSE(save());
  EXPECT_EQ(load(), "old");
  EXPECT_FALSE(model.exists((std::string(TOKEN) + ".bak").c_str()));
}

TEST_F(PluginTokenStorage, SuccessfulSaveKeepsFormatAndEscapedValue) {
  ASSERT_TRUE(save("new\n\"token"));
  EXPECT_EQ(load(), "new\n\"token");
  EXPECT_FALSE(model.exists((std::string(TOKEN) + ".new").c_str()));
  EXPECT_FALSE(model.exists((std::string(TOKEN) + ".bak").c_str()));
}

TEST_F(PluginTokenStorage, InterruptedSaveKeepsACompleteOldOrNewToken) {
  ASSERT_TRUE(save());
  const int steps = model.mutations;
  for (int cut = 1; cut <= steps; ++cut) {
    SCOPED_TRACE(cut);
    model = {};
    model.files[TOKEN] = OLD;
    model.files["/plugins/test/config.json"] = "private settings";
    model.cutAfter = cut;
    try {
      save();
    } catch (const PowerCut&) {
    }
    model.cutAfter = -1;
    const auto token = load();
    EXPECT_TRUE(token == "old" || token == "new");
  }
}

TEST_F(PluginTokenStorage, FailedStageOperationsPreserveOldToken) {
  const std::string stage = std::string(TOKEN) + ".new";
  model.failOpen = stage;
  EXPECT_FALSE(save());
  EXPECT_EQ(load(), "old");
  model.failOpen.clear();
  model.failRead = stage;
  EXPECT_FALSE(save());
  EXPECT_EQ(load(), "old");
  model.failRead.clear();
  model.corruptWrite = true;
  EXPECT_FALSE(save());
  EXPECT_EQ(load(), "old");
}

TEST_F(PluginTokenStorage, FailedRenameKeepsRecoveryCopy) {
  model.failRenameFrom = TOKEN;
  EXPECT_FALSE(save());
  EXPECT_EQ(load(), "old");
  model.failRenameFrom.clear();
  model.failRenameTo = TOKEN;
  EXPECT_FALSE(save());
  EXPECT_EQ(model.files.at(std::string(TOKEN) + ".bak"), OLD);
  model.failRenameTo.clear();
  EXPECT_EQ(load(), "old");
}

TEST_F(PluginTokenStorage, ExistingPrimaryWinsOverBackup) {
  model.files[std::string(TOKEN) + ".bak"] = R"({"auth":{"token":"older"}})";
  EXPECT_EQ(load(), "old");
}

TEST_F(PluginTokenStorage, FailedBackupCleanupDoesNotHideCommittedToken) {
  model.failRemove = std::string(TOKEN) + ".bak";
  ASSERT_TRUE(save());
  EXPECT_EQ(load(), "new");
  EXPECT_FALSE(save("later"));
  EXPECT_EQ(load(), "new");
  model.failRemove.clear();
  EXPECT_EQ(load(), "new");
  EXPECT_FALSE(model.exists((std::string(TOKEN) + ".bak").c_str()));
}

TEST_F(PluginTokenStorage, MissingPrimaryRestoresBackupNotStage) {
  model.files.erase(TOKEN);
  model.files[std::string(TOKEN) + ".bak"] = OLD;
  model.files[std::string(TOKEN) + ".new"] = "truncated";
  EXPECT_EQ(load(), "old");
  EXPECT_EQ(model.files.at(TOKEN), OLD);
}

TEST_F(PluginTokenStorage, FailedRecoveryDoesNotOverwriteBackup) {
  model.files.erase(TOKEN);
  model.files[std::string(TOKEN) + ".bak"] = OLD;
  model.failRenameFrom = std::string(TOKEN) + ".bak";
  EXPECT_FALSE(save());
  std::string token;
  EXPECT_FALSE(pluginhttp::loadTokenFromFile(TOKEN, "auth.token", token));
  EXPECT_EQ(model.files.at(std::string(TOKEN) + ".bak"), OLD);
  EXPECT_EQ(model.mutations, 0);
}

TEST_F(PluginTokenStorage, SaveRecoversAnInterruptedReplacement) {
  model.files.erase(TOKEN);
  model.files[std::string(TOKEN) + ".bak"] = OLD;
  model.files[std::string(TOKEN) + ".new"] = "partial";
  ASSERT_TRUE(save());
  EXPECT_EQ(load(), "new");
}

TEST_F(PluginTokenStorage, InterruptedFirstSaveNeverPromotesPartialStage) {
  model.files.erase(TOKEN);
  ASSERT_TRUE(save());
  const int steps = model.mutations;
  for (int cut = 1; cut <= steps; ++cut) {
    SCOPED_TRACE(cut);
    model = {};
    model.files["/plugins/test/config.json"] = "private settings";
    model.cutAfter = cut;
    try {
      save();
    } catch (const PowerCut&) {
    }
    model.cutAfter = -1;
    std::string token;
    const bool available = pluginhttp::loadTokenFromFile(TOKEN, "auth.token", token);
    EXPECT_EQ(available, token == "new");
    EXPECT_TRUE(token.empty() || token == "new");
  }
}

TEST_F(PluginTokenStorage, InvalidOrExplicitlyEmptyPrimaryDoesNotRestoreOldAccess) {
  model.files[std::string(TOKEN) + ".bak"] = OLD;
  for (const auto* invalid : {"bad json", "{}", R"({"auth":{"token":""}})"}) {
    model.files[TOKEN] = invalid;
    std::string token = "stale";
    EXPECT_FALSE(pluginhttp::loadTokenFromFile(TOKEN, "auth.token", token));
    EXPECT_TRUE(token.empty());
    EXPECT_EQ(model.files.at(TOKEN), invalid);
  }
}

TEST_F(PluginTokenStorage, OversizeWriteDoesNotDestroyReadableToken) {
  EXPECT_FALSE(save(std::string(2048, 'x')));
  EXPECT_EQ(load(), "old");
}

TEST_F(PluginTokenStorage, ProtectedPathsStayUnchanged) {
  model.files["/.crosspoint/wifi.json"] = "credential store";
  EXPECT_FALSE(pluginhttp::saveTokenToFile("/.crosspoint/wifi.json", "token", "new"));
  std::string value;
  EXPECT_FALSE(pluginhttp::loadTokenFromFile("/.crosspoint/wifi.json", "token", value));
  EXPECT_EQ(model.files["/.crosspoint/wifi.json"], "credential store");
  EXPECT_EQ(model.mutations, 0);
}

TEST_F(PluginTokenStorage, EmptyFileKeepsTokenlessContract) {
  std::string value = "old";
  EXPECT_TRUE(pluginhttp::loadTokenFromFile("", "token", value));
  EXPECT_TRUE(value.empty());
  EXPECT_FALSE(pluginhttp::saveTokenToFile("", "token", "new"));
}
}  // namespace
