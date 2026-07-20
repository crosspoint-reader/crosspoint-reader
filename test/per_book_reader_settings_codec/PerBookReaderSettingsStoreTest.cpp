#include <HalStorage.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "PerBookReaderSettingsCodec.h"
#include "PerBookReaderSettingsStore.h"

namespace {

constexpr char CACHE_PATH[] = "/.crosspoint/epub_test";

std::string path(const char* suffix) {
  return std::string(CACHE_PATH) + "/" + PerBookReaderSettingsStore::FILE_NAME + suffix;
}

std::vector<uint8_t> encodedSettings(const uint8_t fontSize = 1) {
  PerBookReaderSettings settings;
  settings.hasReaderOverrides = true;
  settings.fontSize = fontSize;
  PerBookReaderSettingsCodec::Encoded encoded;
  EXPECT_TRUE(PerBookReaderSettingsCodec::encode(settings, encoded));
  return {encoded.begin(), encoded.end()};
}

std::vector<uint8_t> newerSettings() {
  auto encoded = encodedSettings();
  encoded[PerBookReaderSettingsCodec::VERSION_OFFSET] = PerBookReaderSettingsCodec::VERSION + 1;
  return encoded;
}

class PerBookReaderSettingsStoreTest : public testing::Test {
 protected:
  void SetUp() override {
    Storage.reset();
    Storage.mkdir("/.crosspoint");
    Storage.mkdir(CACHE_PATH);
  }
};

TEST_F(PerBookReaderSettingsStoreTest, ClearRemovesOnlyInspectedCurrentOrCorruptFiles) {
  Storage.setFile(path(""), encodedSettings());
  Storage.setFile(path(".tmp"), {0x01, 0x02});
  Storage.setFile(path(".bak"), encodedSettings(2));

  EXPECT_TRUE(PerBookReaderSettingsStore::clear(CACHE_PATH));
  EXPECT_FALSE(Storage.exists(path("").c_str()));
  EXPECT_FALSE(Storage.exists(path(".tmp").c_str()));
  EXPECT_FALSE(Storage.exists(path(".bak").c_str()));
}

TEST_F(PerBookReaderSettingsStoreTest, ClearPreservesEveryFileWhenBackupIsNewer) {
  const auto current = encodedSettings();
  const auto temporary = encodedSettings(2);
  const auto newer = newerSettings();
  Storage.setFile(path(""), current);
  Storage.setFile(path(".tmp"), temporary);
  Storage.setFile(path(".bak"), newer);

  EXPECT_FALSE(PerBookReaderSettingsStore::clear(CACHE_PATH));
  EXPECT_EQ(Storage.file(path("")), current);
  EXPECT_EQ(Storage.file(path(".tmp")), temporary);
  EXPECT_EQ(Storage.file(path(".bak")), newer);
}

TEST_F(PerBookReaderSettingsStoreTest, SaveRefusesNewerTemporaryWithoutChangingCurrent) {
  const auto current = encodedSettings();
  const auto newer = newerSettings();
  Storage.setFile(path(""), current);
  Storage.setFile(path(".tmp"), newer);

  PerBookReaderSettings updated;
  updated.hasReaderOverrides = true;
  updated.fontSize = 3;
  EXPECT_EQ(PerBookReaderSettingsStore::save(CACHE_PATH, updated),
            PerBookReaderSettingsStore::SaveStatus::NEWER_VERSION);
  EXPECT_EQ(Storage.file(path("")), current);
  EXPECT_EQ(Storage.file(path(".tmp")), newer);
}

TEST_F(PerBookReaderSettingsStoreTest, LoadUsesValidTemporaryWhenNoCommittedCopySurvived) {
  const auto temporary = encodedSettings(3);
  Storage.setFile(path(".tmp"), temporary);

  PerBookReaderSettings loaded;
  EXPECT_EQ(PerBookReaderSettingsStore::load(CACHE_PATH, loaded), PerBookReaderSettingsStore::LoadStatus::LOADED_TEMP);
  EXPECT_TRUE(loaded.hasReaderOverrides);
  EXPECT_EQ(loaded.fontSize, 3);
  EXPECT_EQ(Storage.file(path(".tmp")), temporary);
}

TEST_F(PerBookReaderSettingsStoreTest, LoadPrefersCommittedBackupOverTemporaryCopy) {
  Storage.setFile(path(".bak"), encodedSettings(2));
  Storage.setFile(path(".tmp"), encodedSettings(3));

  PerBookReaderSettings loaded;
  EXPECT_EQ(PerBookReaderSettingsStore::load(CACHE_PATH, loaded),
            PerBookReaderSettingsStore::LoadStatus::LOADED_BACKUP);
  EXPECT_EQ(loaded.fontSize, 2);
}

TEST_F(PerBookReaderSettingsStoreTest, LoadDoesNotFallBackPastAnUnreadableCommittedCopy) {
  const auto primary = encodedSettings(1);
  const auto backup = encodedSettings(2);
  Storage.setFile(path(""), primary);
  Storage.setFile(path(".bak"), backup);
  Storage.makeUnreadable(path(""));

  PerBookReaderSettings loaded;
  EXPECT_EQ(PerBookReaderSettingsStore::load(CACHE_PATH, loaded), PerBookReaderSettingsStore::LoadStatus::IO_ERROR);
  EXPECT_EQ(Storage.file(path("")), primary);
  EXPECT_EQ(Storage.file(path(".bak")), backup);
}

TEST_F(PerBookReaderSettingsStoreTest, LoadRefusesNewerTemporaryWhenNoCommittedCopyExists) {
  const auto newer = newerSettings();
  Storage.setFile(path(".tmp"), newer);

  PerBookReaderSettings loaded;
  EXPECT_EQ(PerBookReaderSettingsStore::load(CACHE_PATH, loaded),
            PerBookReaderSettingsStore::LoadStatus::NEWER_VERSION);
  EXPECT_EQ(Storage.file(path(".tmp")), newer);
}

TEST_F(PerBookReaderSettingsStoreTest, FailedCanonicalRotationKeepsPreviousCanonical) {
  const auto current = encodedSettings();
  Storage.setFile(path(""), current);
  Storage.failRenameOnce();

  PerBookReaderSettings updated;
  updated.hasReaderOverrides = true;
  updated.fontSize = 3;
  EXPECT_EQ(PerBookReaderSettingsStore::save(CACHE_PATH, updated), PerBookReaderSettingsStore::SaveStatus::IO_ERROR);
  EXPECT_EQ(Storage.file(path("")), current);
  EXPECT_FALSE(Storage.exists(path(".tmp").c_str()));
}

TEST_F(PerBookReaderSettingsStoreTest, CorruptFirstPromotionIsNeverReportedAsSaved) {
  Storage.corruptRenameOnce();
  PerBookReaderSettings settings;
  settings.hasReaderOverrides = true;
  settings.fontSize = 3;

  EXPECT_EQ(PerBookReaderSettingsStore::save(CACHE_PATH, settings), PerBookReaderSettingsStore::SaveStatus::IO_ERROR);
  EXPECT_TRUE(Storage.exists(path(".tmp").c_str()));

  PerBookReaderSettings loaded;
  EXPECT_EQ(PerBookReaderSettingsStore::load(CACHE_PATH, loaded), PerBookReaderSettingsStore::LoadStatus::LOADED_TEMP);
  EXPECT_EQ(loaded, settings);
}

}  // namespace
