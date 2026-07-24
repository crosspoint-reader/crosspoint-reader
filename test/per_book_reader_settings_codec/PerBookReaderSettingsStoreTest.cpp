#include <HalStorage.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "PerBookReaderSettingsCodec.h"
#include "PerBookReaderSettingsStore.h"

namespace {

constexpr char CACHE_PATH[] = "/.crosspoint/epub_test";

std::string path(const char* suffix) {
  return std::string(CACHE_PATH) + "/" + PerBookReaderSettingsStore::FILE_NAME + suffix;
}

std::string legacyPath(const std::string_view suffix = {}) {
  return std::string(CACHE_PATH) + "/" + PerBookReaderSettingsStore::CROSSINK_FILE_NAME + std::string(suffix);
}

std::string legacyBackupPath(const uint8_t version, const std::string_view suffix = {}) {
  return legacyPath(std::string(".crossink-v") + std::to_string(version) + ".orig" + std::string(suffix));
}

std::vector<uint8_t> crossInkV1(const uint16_t seconds = 30) {
  return {1, static_cast<uint8_t>(seconds), static_cast<uint8_t>(seconds >> 8)};
}

std::vector<uint8_t> crossInkV2(const uint8_t flags = 0x07, const uint16_t seconds = 37,
                                const std::string_view fontName = "Legacy Việt") {
  std::vector<uint8_t> bytes(86, 0);
  bytes[0] = 2;
  bytes[1] = flags;
  bytes[2] = static_cast<uint8_t>(seconds);
  bytes[3] = static_cast<uint8_t>(seconds >> 8);
  bytes[4] = 2;
  bytes[5] = 0;
  bytes[6] = 7;
  bytes[7] = 110;
  bytes[8] = 3;
  bytes[9] = 40;
  bytes[10] = 1;
  bytes[11] = 4;
  bytes[12] = 0;
  bytes[13] = 1;
  bytes[14] = 0;
  bytes[15] = 1;
  bytes[16] = 2;
  bytes[17] = 0;
  bytes[18] = 1;
  bytes[19] = 1;
  bytes[20] = 1;
  bytes[21] = 2;
  std::copy(fontName.begin(), fontName.end(), bytes.begin() + 22);
  return bytes;
}

PerBookReaderSettings globalDefaults() {
  PerBookReaderSettings settings;
  settings.fontFamily = 1;
  settings.fontSize = 3;
  settings.lineSpacing = 0;
  settings.orientation = 1;
  settings.screenMargin = 15;
  setPerBookSdFontFamilyName(settings, "Global Font");
  return settings;
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

TEST_F(PerBookReaderSettingsStoreTest, MigratesCrossInkV1WithoutAutoStartingAndKeepsImmutableOriginal) {
  const auto legacy = crossInkV1(45);
  Storage.setFile(legacyPath(), legacy);

  EXPECT_EQ(PerBookReaderSettingsStore::migrateCrossInk(CACHE_PATH, globalDefaults()),
            PerBookReaderSettingsStore::MigrationStatus::MIGRATED);
  EXPECT_EQ(Storage.file(legacyPath()), legacy);
  EXPECT_EQ(Storage.file(legacyBackupPath(1)), legacy);
  EXPECT_FALSE(Storage.exists(legacyBackupPath(1, ".tmp").c_str()));

  PerBookReaderSettings loaded;
  ASSERT_EQ(PerBookReaderSettingsStore::load(CACHE_PATH, loaded), PerBookReaderSettingsStore::LoadStatus::LOADED);
  EXPECT_FALSE(loaded.hasReaderOverrides);
  EXPECT_TRUE(loaded.hasAutoPageTurnInterval);
  EXPECT_FALSE(loaded.autoPageTurnStartsOnOpen);
  EXPECT_EQ(loaded.autoPageTurnSeconds, 45);
  EXPECT_EQ(loaded.fontFamily, globalDefaults().fontFamily);
  EXPECT_EQ(loaded.sdFontFamilyName, globalDefaults().sdFontFamilyName);
}

TEST_F(PerBookReaderSettingsStoreTest, MigratesCrossInkZeroIntervalAsDisabled) {
  for (const auto& legacy : {crossInkV1(0), crossInkV2(0x07, 0)}) {
    Storage.reset();
    Storage.mkdir("/.crosspoint");
    Storage.mkdir(CACHE_PATH);
    Storage.setFile(legacyPath(), legacy);

    ASSERT_EQ(PerBookReaderSettingsStore::migrateCrossInk(CACHE_PATH, globalDefaults()),
              PerBookReaderSettingsStore::MigrationStatus::MIGRATED);
    PerBookReaderSettings loaded;
    ASSERT_EQ(PerBookReaderSettingsStore::load(CACHE_PATH, loaded), PerBookReaderSettingsStore::LoadStatus::LOADED);
    EXPECT_FALSE(loaded.hasAutoPageTurnInterval);
    EXPECT_FALSE(loaded.autoPageTurnStartsOnOpen);
    EXPECT_EQ(loaded.autoPageTurnSeconds, 0);
    EXPECT_EQ(Storage.file(legacyPath()), legacy);
    EXPECT_EQ(Storage.file(legacyBackupPath(legacy.front())), legacy);
  }
}

TEST_F(PerBookReaderSettingsStoreTest, MapsOnlySemanticallyEquivalentCrossInkV2Fields) {
  const auto legacy = crossInkV2();
  Storage.setFile(legacyPath(), legacy);

  ASSERT_EQ(PerBookReaderSettingsStore::migrateCrossInk(CACHE_PATH, globalDefaults()),
            PerBookReaderSettingsStore::MigrationStatus::MIGRATED);
  PerBookReaderSettings loaded;
  ASSERT_EQ(PerBookReaderSettingsStore::load(CACHE_PATH, loaded), PerBookReaderSettingsStore::LoadStatus::LOADED);
  EXPECT_TRUE(loaded.hasReaderOverrides);
  EXPECT_TRUE(loaded.hasAutoPageTurnInterval);
  EXPECT_FALSE(loaded.autoPageTurnStartsOnOpen);
  EXPECT_EQ(loaded.autoPageTurnSeconds, 37);
  EXPECT_EQ(loaded.fontFamily, globalDefaults().fontFamily);
  EXPECT_EQ(loaded.fontSize, globalDefaults().fontSize);
  EXPECT_EQ(loaded.sdFontFamilyName, globalDefaults().sdFontFamilyName);
  EXPECT_EQ(loaded.lineSpacing, 2);
  EXPECT_EQ(loaded.orientation, 3);
  EXPECT_EQ(loaded.screenMargin, 40);
  EXPECT_EQ(loaded.paragraphAlignment, 4);
  EXPECT_EQ(loaded.embeddedStyle, 0);
  EXPECT_EQ(loaded.hyphenationEnabled, 1);
  EXPECT_EQ(loaded.textAntiAliasing, 0);
  EXPECT_EQ(loaded.imageRendering, 2);
  EXPECT_EQ(loaded.extraParagraphSpacing, 0);
  EXPECT_EQ(loaded.forceParagraphIndents, 1);
  EXPECT_EQ(loaded.focusReadingEnabled, 1);
  EXPECT_TRUE(loaded.hasRenderModeOverride);
  EXPECT_EQ(loaded.renderMode, EpubRenderMode::Light);
  EXPECT_EQ(Storage.file(legacyPath()), legacy);
  EXPECT_EQ(Storage.file(legacyBackupPath(2)), legacy);
}

TEST_F(PerBookReaderSettingsStoreTest, IntervalAndOverridesRemainIndependent) {
  const auto legacy = crossInkV2(0, 30);
  Storage.setFile(legacyPath(), legacy);
  ASSERT_EQ(PerBookReaderSettingsStore::migrateCrossInk(CACHE_PATH, globalDefaults()),
            PerBookReaderSettingsStore::MigrationStatus::MIGRATED);

  PerBookReaderSettings loaded;
  ASSERT_EQ(PerBookReaderSettingsStore::load(CACHE_PATH, loaded), PerBookReaderSettingsStore::LoadStatus::LOADED);
  EXPECT_FALSE(loaded.hasReaderOverrides);
  EXPECT_FALSE(loaded.hasAutoPageTurnInterval);
  EXPECT_FALSE(loaded.autoPageTurnStartsOnOpen);
  EXPECT_EQ(loaded.autoPageTurnSeconds, 0);
}

TEST_F(PerBookReaderSettingsStoreTest, DisablingAndRestoringTypographyPreservesAutoTurnPreference) {
  PerBookReaderSettings custom = globalDefaults();
  custom.hasReaderOverrides = true;
  custom.fontSize = 3;
  custom.hasAutoPageTurnInterval = true;
  custom.autoPageTurnSeconds = 45;
  custom.autoPageTurnStartsOnOpen = true;
  ASSERT_EQ(PerBookReaderSettingsStore::save(CACHE_PATH, custom), PerBookReaderSettingsStore::SaveStatus::SAVED);

  PerBookReaderSettings disabled = custom;
  disabled.hasReaderOverrides = false;
  ASSERT_EQ(PerBookReaderSettingsStore::save(CACHE_PATH, disabled), PerBookReaderSettingsStore::SaveStatus::SAVED);
  PerBookReaderSettings loaded;
  ASSERT_EQ(PerBookReaderSettingsStore::load(CACHE_PATH, loaded), PerBookReaderSettingsStore::LoadStatus::LOADED);
  EXPECT_FALSE(loaded.hasReaderOverrides);
  EXPECT_EQ(loaded.fontSize, 3);
  EXPECT_TRUE(loaded.hasAutoPageTurnInterval);
  EXPECT_EQ(loaded.autoPageTurnSeconds, 45);
  EXPECT_TRUE(loaded.autoPageTurnStartsOnOpen);

  loaded.hasReaderOverrides = true;
  ASSERT_EQ(PerBookReaderSettingsStore::save(CACHE_PATH, loaded), PerBookReaderSettingsStore::SaveStatus::SAVED);
  PerBookReaderSettings restored;
  ASSERT_EQ(PerBookReaderSettingsStore::load(CACHE_PATH, restored), PerBookReaderSettingsStore::LoadStatus::LOADED);
  EXPECT_EQ(restored, custom);
}

TEST_F(PerBookReaderSettingsStoreTest, MigrationIsIdempotentAndNeverOverwritesAnyCrossViCandidate) {
  const auto legacy = crossInkV2();
  Storage.setFile(legacyPath(), legacy);
  ASSERT_EQ(PerBookReaderSettingsStore::migrateCrossInk(CACHE_PATH, globalDefaults()),
            PerBookReaderSettingsStore::MigrationStatus::MIGRATED);
  const auto current = Storage.file(path(""));
  const auto backup = Storage.file(legacyBackupPath(2));

  EXPECT_EQ(PerBookReaderSettingsStore::migrateCrossInk(CACHE_PATH, globalDefaults()),
            PerBookReaderSettingsStore::MigrationStatus::CROSSVI_FILE_PRESENT);
  EXPECT_EQ(Storage.file(path("")), current);
  EXPECT_EQ(Storage.file(legacyBackupPath(2)), backup);

  for (const char* suffix : {"", ".bak", ".tmp"}) {
    Storage.reset();
    Storage.mkdir("/.crosspoint");
    Storage.mkdir(CACHE_PATH);
    Storage.setFile(legacyPath(), legacy);
    Storage.setFile(path(suffix), {0xA5});
    EXPECT_EQ(PerBookReaderSettingsStore::migrateCrossInk(CACHE_PATH, globalDefaults()),
              PerBookReaderSettingsStore::MigrationStatus::CROSSVI_FILE_PRESENT)
        << suffix;
    EXPECT_EQ(Storage.file(path(suffix)), std::vector<uint8_t>{0xA5});
    EXPECT_FALSE(Storage.exists(legacyBackupPath(2).c_str()));
  }
}

TEST_F(PerBookReaderSettingsStoreTest, RejectsNewerCorruptAndAmbiguousCrossInkFilesWithoutWriting) {
  std::vector<std::vector<uint8_t>> invalid;
  invalid.push_back({});
  invalid.push_back({1, 30});
  invalid.push_back({1, 30, 0, 0});
  invalid.push_back(crossInkV1(4));
  invalid.push_back(crossInkV1(121));
  auto bytes = crossInkV2();
  bytes.pop_back();
  invalid.push_back(bytes);
  bytes = crossInkV2();
  bytes.push_back(0);
  invalid.push_back(bytes);
  bytes = crossInkV2();
  bytes[1] = 0x80;
  invalid.push_back(bytes);
  const std::array<std::pair<size_t, uint8_t>, 18> invalidFields = {
      std::pair{size_t{4}, uint8_t{3}},  std::pair{size_t{5}, uint8_t{2}},  std::pair{size_t{6}, uint8_t{8}},
      std::pair{size_t{7}, uint8_t{69}}, std::pair{size_t{8}, uint8_t{4}},  std::pair{size_t{9}, uint8_t{41}},
      std::pair{size_t{10}, uint8_t{2}}, std::pair{size_t{11}, uint8_t{5}}, std::pair{size_t{12}, uint8_t{2}},
      std::pair{size_t{13}, uint8_t{2}}, std::pair{size_t{14}, uint8_t{2}}, std::pair{size_t{15}, uint8_t{2}},
      std::pair{size_t{16}, uint8_t{3}}, std::pair{size_t{17}, uint8_t{2}}, std::pair{size_t{18}, uint8_t{2}},
      std::pair{size_t{19}, uint8_t{2}}, std::pair{size_t{20}, uint8_t{2}}, std::pair{size_t{21}, uint8_t{3}},
  };
  for (const auto& [offset, value] : invalidFields) {
    bytes = crossInkV2();
    bytes[offset] = value;
    invalid.push_back(bytes);
  }
  bytes = crossInkV2();
  bytes[22] = 0xC0;
  bytes[23] = 0;
  invalid.push_back(bytes);
  bytes = crossInkV2();
  std::fill(bytes.begin() + 22, bytes.end(), 'x');
  invalid.push_back(bytes);
  bytes = crossInkV2();
  bytes[40] = 0;
  bytes[41] = 'x';
  invalid.push_back(bytes);

  for (const auto& value : invalid) {
    Storage.reset();
    Storage.mkdir("/.crosspoint");
    Storage.mkdir(CACHE_PATH);
    Storage.setFile(legacyPath(), value);
    EXPECT_EQ(PerBookReaderSettingsStore::migrateCrossInk(CACHE_PATH, globalDefaults()),
              PerBookReaderSettingsStore::MigrationStatus::INVALID_LEGACY_FILE)
        << value.size();
    EXPECT_EQ(Storage.file(legacyPath()), value);
    EXPECT_FALSE(Storage.exists(path("").c_str()));
  }

  Storage.reset();
  Storage.mkdir("/.crosspoint");
  Storage.mkdir(CACHE_PATH);
  Storage.setFile(legacyPath(), {3});
  EXPECT_EQ(PerBookReaderSettingsStore::migrateCrossInk(CACHE_PATH, globalDefaults()),
            PerBookReaderSettingsStore::MigrationStatus::NEWER_CROSSINK_VERSION);
  EXPECT_EQ(Storage.file(legacyPath()), std::vector<uint8_t>{3});
}

TEST_F(PerBookReaderSettingsStoreTest, ReusesOnlyAnExactVersionedBackup) {
  const auto legacy = crossInkV2();
  Storage.setFile(legacyPath(), legacy);
  Storage.setFile(legacyBackupPath(2), legacy);
  EXPECT_EQ(PerBookReaderSettingsStore::migrateCrossInk(CACHE_PATH, globalDefaults()),
            PerBookReaderSettingsStore::MigrationStatus::MIGRATED);
  EXPECT_EQ(Storage.file(legacyBackupPath(2)), legacy);

  Storage.reset();
  Storage.mkdir("/.crosspoint");
  Storage.mkdir(CACHE_PATH);
  Storage.setFile(legacyPath(), legacy);
  Storage.setFile(legacyBackupPath(2), crossInkV2(0x07, 38));
  EXPECT_EQ(PerBookReaderSettingsStore::migrateCrossInk(CACHE_PATH, globalDefaults()),
            PerBookReaderSettingsStore::MigrationStatus::BACKUP_CONFLICT);
  EXPECT_EQ(Storage.file(legacyPath()), legacy);
  EXPECT_FALSE(Storage.exists(path("").c_str()));

  Storage.reset();
  Storage.mkdir("/.crosspoint");
  Storage.mkdir(CACHE_PATH);
  Storage.setFile(legacyPath(), legacy);
  Storage.setFile(legacyBackupPath(2), legacy);
  Storage.setFile(legacyBackupPath(2, ".tmp"), crossInkV2(0x07, 38));
  EXPECT_EQ(PerBookReaderSettingsStore::migrateCrossInk(CACHE_PATH, globalDefaults()),
            PerBookReaderSettingsStore::MigrationStatus::BACKUP_CONFLICT);
  EXPECT_EQ(Storage.file(legacyPath()), legacy);
  EXPECT_FALSE(Storage.exists(path("").c_str()));
}

TEST_F(PerBookReaderSettingsStoreTest, MissingLegacyAndInvalidDefaultsDoNotCreateFiles) {
  EXPECT_EQ(PerBookReaderSettingsStore::migrateCrossInk(CACHE_PATH, globalDefaults()),
            PerBookReaderSettingsStore::MigrationStatus::NO_LEGACY_FILE);

  const auto legacy = crossInkV1();
  Storage.setFile(legacyPath(), legacy);
  auto invalidDefaults = globalDefaults();
  invalidDefaults.screenMargin = 0;
  EXPECT_EQ(PerBookReaderSettingsStore::migrateCrossInk(CACHE_PATH, invalidDefaults),
            PerBookReaderSettingsStore::MigrationStatus::INVALID_DEFAULTS);
  EXPECT_EQ(Storage.file(legacyPath()), legacy);
  EXPECT_FALSE(Storage.exists(path("").c_str()));
  EXPECT_FALSE(Storage.exists(legacyBackupPath(1).c_str()));
}

TEST_F(PerBookReaderSettingsStoreTest, BackupCreationFaultsPreserveLegacyAndCanResumeExactTemporary) {
  const auto legacy = crossInkV1();
  for (const auto inject : {0, 1}) {
    Storage.reset();
    Storage.mkdir("/.crosspoint");
    Storage.mkdir(CACHE_PATH);
    Storage.setFile(legacyPath(), legacy);
    if (inject == 0) {
      Storage.shortWriteOnce();
    } else {
      Storage.failSyncOnce();
    }
    EXPECT_EQ(PerBookReaderSettingsStore::migrateCrossInk(CACHE_PATH, globalDefaults()),
              PerBookReaderSettingsStore::MigrationStatus::IO_ERROR);
    EXPECT_EQ(Storage.file(legacyPath()), legacy);
    EXPECT_FALSE(Storage.exists(path("").c_str()));
  }

  Storage.reset();
  Storage.mkdir("/.crosspoint");
  Storage.mkdir(CACHE_PATH);
  Storage.setFile(legacyPath(), legacy);
  Storage.failRenameOnce();
  EXPECT_EQ(PerBookReaderSettingsStore::migrateCrossInk(CACHE_PATH, globalDefaults()),
            PerBookReaderSettingsStore::MigrationStatus::IO_ERROR);
  EXPECT_EQ(Storage.file(legacyBackupPath(1, ".tmp")), legacy);
  EXPECT_EQ(PerBookReaderSettingsStore::migrateCrossInk(CACHE_PATH, globalDefaults()),
            PerBookReaderSettingsStore::MigrationStatus::MIGRATED);
  EXPECT_EQ(Storage.file(legacyBackupPath(1)), legacy);
}

TEST_F(PerBookReaderSettingsStoreTest, InterruptedCrossViPublishLeavesImmutableBackupAndRetriesSafely) {
  const auto legacy = crossInkV2();
  Storage.setFile(legacyPath(), legacy);
  Storage.failRenameOnCalls({2});
  EXPECT_EQ(PerBookReaderSettingsStore::migrateCrossInk(CACHE_PATH, globalDefaults()),
            PerBookReaderSettingsStore::MigrationStatus::SAVE_FAILED);
  EXPECT_EQ(Storage.file(legacyPath()), legacy);
  EXPECT_EQ(Storage.file(legacyBackupPath(2)), legacy);
  EXPECT_FALSE(Storage.exists(path("").c_str()));
  EXPECT_FALSE(Storage.exists(path(".tmp").c_str()));

  EXPECT_EQ(PerBookReaderSettingsStore::migrateCrossInk(CACHE_PATH, globalDefaults()),
            PerBookReaderSettingsStore::MigrationStatus::MIGRATED);
  EXPECT_EQ(Storage.file(legacyBackupPath(2)), legacy);
}

TEST_F(PerBookReaderSettingsStoreTest, ResetCrossViProfileNeverDeletesCrossInkDataOrBackup) {
  const auto legacy = crossInkV2();
  Storage.setFile(legacyPath(), legacy);
  ASSERT_EQ(PerBookReaderSettingsStore::migrateCrossInk(CACHE_PATH, globalDefaults()),
            PerBookReaderSettingsStore::MigrationStatus::MIGRATED);

  EXPECT_TRUE(PerBookReaderSettingsStore::clear(CACHE_PATH));
  EXPECT_EQ(Storage.file(legacyPath()), legacy);
  EXPECT_EQ(Storage.file(legacyBackupPath(2)), legacy);
  EXPECT_FALSE(Storage.exists(path("").c_str()));
}

TEST_F(PerBookReaderSettingsStoreTest, DisabledCrossViRecordPreventsImportingLegacySettingsAgain) {
  const auto legacy = crossInkV2();
  Storage.setFile(legacyPath(), legacy);
  ASSERT_EQ(PerBookReaderSettingsStore::migrateCrossInk(CACHE_PATH, globalDefaults()),
            PerBookReaderSettingsStore::MigrationStatus::MIGRATED);

  const PerBookReaderSettings disabled = globalDefaults();
  ASSERT_EQ(PerBookReaderSettingsStore::save(CACHE_PATH, disabled), PerBookReaderSettingsStore::SaveStatus::SAVED);
  EXPECT_EQ(PerBookReaderSettingsStore::migrateCrossInk(CACHE_PATH, globalDefaults()),
            PerBookReaderSettingsStore::MigrationStatus::CROSSVI_FILE_PRESENT);

  PerBookReaderSettings loaded;
  ASSERT_EQ(PerBookReaderSettingsStore::load(CACHE_PATH, loaded), PerBookReaderSettingsStore::LoadStatus::LOADED);
  EXPECT_EQ(loaded, disabled);
  EXPECT_EQ(Storage.file(legacyPath()), legacy);
  EXPECT_EQ(Storage.file(legacyBackupPath(2)), legacy);
}

}  // namespace
