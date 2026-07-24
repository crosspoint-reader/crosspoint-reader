#include <gtest/gtest.h>

#include <AtomicFile.h>
#include <ClockCalendar.h>
#include <util/ClockSyncPolicy.h>
#include <HalStorage.h>
#include <LegacyStateCodec.h>
#include <StagedFileTransaction.h>
#include <TiltLifecyclePolicy.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr char PATH[] = "/state.json";

std::vector<uint8_t> bytes(const std::string& value) { return {value.begin(), value.end()}; }

bool objectValidator(const uint8_t* data, const size_t size, void*) {
  return size >= 2 && data[0] == '{' && data[size - 1] == '}';
}

AtomicFile::SaveStatus save(const std::string& value, const size_t maxSize = 64) {
  return AtomicFile::save(PATH, reinterpret_cast<const uint8_t*>(value.data()), value.size(), maxSize,
                          objectValidator);
}

AtomicFile::LoadStatus load(std::string& value, const size_t maxSize = 64) {
  return AtomicFile::load(PATH, value, maxSize, objectValidator);
}

void expectRecoverable(const std::string& oldValue, const std::string& newValue) {
  std::string recovered;
  const auto status = load(recovered);
  EXPECT_TRUE(status == AtomicFile::LoadStatus::Primary || status == AtomicFile::LoadStatus::Backup ||
              status == AtomicFile::LoadStatus::Temp);
  EXPECT_TRUE(recovered == oldValue || recovered == newValue);
}

class AtomicPersistenceTest : public testing::Test {
 protected:
  void SetUp() override { Storage.reset(); }
};

TEST_F(AtomicPersistenceTest, PublishesAndRotatesVerifiedJson) {
  ASSERT_EQ(save("{old}"), AtomicFile::SaveStatus::Saved);
  ASSERT_EQ(save("{new}"), AtomicFile::SaveStatus::Saved);
  std::string loaded;
  EXPECT_EQ(load(loaded), AtomicFile::LoadStatus::Primary);
  EXPECT_EQ(loaded, "{new}");
  EXPECT_EQ(std::string(Storage.file(PATH).begin(), Storage.file(PATH).end()), "{new}");
  EXPECT_EQ(std::string(Storage.file("/state.json.bak").begin(), Storage.file("/state.json.bak").end()), "{old}");
}

TEST_F(AtomicPersistenceTest, RejectsPayloadOverByteLimitBeforeOpeningFile) {
  EXPECT_EQ(save("{123}", 4), AtomicFile::SaveStatus::Oversize);
  EXPECT_FALSE(Storage.exists(PATH));
  EXPECT_EQ(save("{}", 2), AtomicFile::SaveStatus::Saved);
}

TEST_F(AtomicPersistenceTest, OpenFailureRetainsPrimary) {
  Storage.setFile(PATH, bytes("{old}"));
  Storage.makeUnwritable("/state.json.tmp");
  EXPECT_EQ(save("{new}"), AtomicFile::SaveStatus::IoError);
  expectRecoverable("{old}", "{new}");
}

TEST_F(AtomicPersistenceTest, ShortWriteRetainsPrimary) {
  Storage.setFile(PATH, bytes("{old}"));
  Storage.shortWriteFor("/state.json.tmp");
  EXPECT_EQ(save("{new}"), AtomicFile::SaveStatus::IoError);
  expectRecoverable("{old}", "{new}");
}

TEST_F(AtomicPersistenceTest, SyncFailureRetainsPrimary) {
  Storage.setFile(PATH, bytes("{old}"));
  Storage.failSyncOnce();
  EXPECT_EQ(save("{new}"), AtomicFile::SaveStatus::IoError);
  expectRecoverable("{old}", "{new}");
}

TEST_F(AtomicPersistenceTest, CloseFailureRetainsPrimary) {
  Storage.setFile(PATH, bytes("{old}"));
  Storage.failCloseFor("/state.json.tmp");
  EXPECT_EQ(save("{new}"), AtomicFile::SaveStatus::IoError);
  expectRecoverable("{old}", "{new}");
}

TEST_F(AtomicPersistenceTest, BackupRenameFailureRetainsPrimary) {
  Storage.setFile(PATH, bytes("{old}"));
  Storage.failRenameOnce();
  EXPECT_EQ(save("{new}"), AtomicFile::SaveStatus::IoError);
  expectRecoverable("{old}", "{new}");
}

TEST_F(AtomicPersistenceTest, PublishRenameFailureLeavesVerifiedBackupOrTemp) {
  Storage.failRenameOnce();
  EXPECT_EQ(save("{new}"), AtomicFile::SaveStatus::IoError);
  expectRecoverable("{old}", "{new}");
}

TEST_F(AtomicPersistenceTest, PublishVerificationFailureLeavesVerifiedBackup) {
  Storage.corruptRenameOnce();
  EXPECT_EQ(save("{new}"), AtomicFile::SaveStatus::IoError);
  expectRecoverable("{old}", "{new}");
}

TEST_F(AtomicPersistenceTest, RecoversTempWhenItIsOnlyValidCandidate) {
  Storage.setFile("/state.json.tmp", bytes("{new}"));
  std::string loaded;
  EXPECT_EQ(load(loaded), AtomicFile::LoadStatus::Temp);
  EXPECT_EQ(loaded, "{new}");
}

TEST_F(AtomicPersistenceTest, RecoversBackupAfterMalformedPrimary) {
  Storage.setFile(PATH, bytes("bad"));
  Storage.setFile("/state.json.bak", bytes("{old}"));
  std::string loaded;
  EXPECT_EQ(load(loaded), AtomicFile::LoadStatus::Backup);
  EXPECT_EQ(loaded, "{old}");
}

TEST_F(AtomicPersistenceTest, PrefersGoodPrimaryOverMalformedTemp) {
  Storage.setFile(PATH, bytes("{old}"));
  Storage.setFile("/state.json.tmp", bytes("bad"));
  std::string loaded;
  EXPECT_EQ(load(loaded), AtomicFile::LoadStatus::Primary);
  EXPECT_EQ(loaded, "{old}");
}

template <typename T>
void append(std::vector<uint8_t>& output, const T& value) {
  const auto* raw = reinterpret_cast<const uint8_t*>(&value);
  output.insert(output.end(), raw, raw + sizeof(value));
}

std::vector<uint8_t> legacyState(const uint8_t version, const std::string& path = "/book.epub") {
  std::vector<uint8_t> output;
  append(output, version);
  const uint32_t length = path.size();
  append(output, length);
  output.insert(output.end(), path.begin(), path.end());
  if (version >= 2) append(output, static_cast<uint8_t>(7));
  if (version >= 3) append(output, static_cast<uint8_t>(9));
  if (version >= 4) append(output, true);
  return output;
}

LegacyStateCodec::DecodeStatus decodeLegacy(const std::vector<uint8_t>& encoded, LegacyStateCodec::State& state) {
  Storage.setFile("/state.bin", encoded);
  HalFile file;
  EXPECT_TRUE(Storage.openFileForRead("TEST", "/state.bin", file));
  return LegacyStateCodec::decode(file, state);
}

TEST_F(AtomicPersistenceTest, ValidLegacyStateRemainsCompatible) {
  LegacyStateCodec::State state;
  EXPECT_EQ(decodeLegacy(legacyState(4), state), LegacyStateCodec::DecodeStatus::Ok);
  EXPECT_EQ(state.openBookPath, "/book.epub");
  EXPECT_EQ(state.lastSleepImage, 7);
  EXPECT_EQ(state.readerActivityLoadCount, 9);
  EXPECT_TRUE(state.lastSleepFromReader);
}

TEST_F(AtomicPersistenceTest, EveryTruncationIsRejectedWithoutPublishingPartialState) {
  const auto valid = legacyState(4);
  for (size_t size = 0; size < valid.size(); ++size) {
    LegacyStateCodec::State state;
    state.openBookPath = "sentinel";
    const std::vector<uint8_t> truncated(valid.begin(), valid.begin() + size);
    EXPECT_NE(decodeLegacy(truncated, state), LegacyStateCodec::DecodeStatus::Ok) << size;
    EXPECT_EQ(state.openBookPath, "sentinel") << size;
  }
}

TEST_F(AtomicPersistenceTest, UntrustedLegacyLengthsAreRejectedBeforeAllocation) {
  std::vector<uint8_t> encoded{4};
  append(encoded, UINT32_MAX);
  LegacyStateCodec::State state;
  EXPECT_EQ(decodeLegacy(encoded, state), LegacyStateCodec::DecodeStatus::Invalid);

  encoded = {4};
  append(encoded, static_cast<uint32_t>(100));
  encoded.push_back('x');
  EXPECT_EQ(decodeLegacy(encoded, state), LegacyStateCodec::DecodeStatus::Invalid);
}

TEST_F(AtomicPersistenceTest, FutureLegacyVersionIsRejected) {
  LegacyStateCodec::State state;
  EXPECT_EQ(decodeLegacy(legacyState(5), state), LegacyStateCodec::DecodeStatus::FutureVersion);
}

TEST_F(AtomicPersistenceTest, LegacyShortReadIsReportedAsIoError) {
  Storage.setFile("/state.bin", legacyState(4));
  Storage.shortReadFor("/state.bin");
  HalFile file;
  ASSERT_TRUE(Storage.openFileForRead("TEST", "/state.bin", file));
  LegacyStateCodec::State state;
  EXPECT_EQ(LegacyStateCodec::decode(file, state), LegacyStateCodec::DecodeStatus::IoError);
}

bool fontValidator(const char* path, void*) {
  if (!Storage.exists(path)) return false;
  const auto& data = Storage.file(path);
  return data.size() >= 8 && memcmp(data.data(), "CPFONT\0\0", 8) == 0;
}

constexpr char FONT[] = "/fonts/Regular.cpfont";
constexpr char FONT_TEMP[] = "/fonts/Regular.cpfont.upload.tmp";
constexpr char FONT_BACKUP[] = "/fonts/Regular.cpfont.upload.bak";

std::vector<uint8_t> fontBytes(const uint8_t marker) {
  std::vector<uint8_t> output{'C', 'P', 'F', 'O', 'N', 'T', 0, 0};
  output.push_back(marker);
  return output;
}

TEST_F(AtomicPersistenceTest, FontPublishKeepsOldUntilNewFileVerifies) {
  Storage.setFile(FONT, fontBytes(1));
  Storage.setFile(FONT_TEMP, fontBytes(2));
  EXPECT_EQ(StagedFileTransaction::publish(FONT, FONT_TEMP, FONT_BACKUP, fontValidator),
            StagedFileTransaction::Status::Published);
  EXPECT_EQ(Storage.file(FONT).back(), 2);
  EXPECT_FALSE(Storage.exists(FONT_BACKUP));
}

TEST_F(AtomicPersistenceTest, InvalidFontStagingNeverTouchesOldFont) {
  Storage.setFile(FONT, fontBytes(1));
  Storage.setFile(FONT_TEMP, bytes("invalid"));
  EXPECT_EQ(StagedFileTransaction::publish(FONT, FONT_TEMP, FONT_BACKUP, fontValidator),
            StagedFileTransaction::Status::InvalidStaging);
  EXPECT_EQ(Storage.file(FONT).back(), 1);
}

TEST_F(AtomicPersistenceTest, FontPublishRenameFailureRestoresOldFont) {
  Storage.setFile(FONT, fontBytes(1));
  Storage.setFile(FONT_TEMP, fontBytes(2));
  Storage.failRenameTo(FONT);
  EXPECT_EQ(StagedFileTransaction::publish(FONT, FONT_TEMP, FONT_BACKUP, fontValidator),
            StagedFileTransaction::Status::IoError);
  EXPECT_TRUE(Storage.exists(FONT));
  EXPECT_EQ(Storage.file(FONT).back(), 1);
}

TEST_F(AtomicPersistenceTest, FontPostPublishVerifyFailureRestoresOldFont) {
  Storage.setFile(FONT, fontBytes(1));
  Storage.setFile(FONT_TEMP, fontBytes(2));
  Storage.corruptRenameTo(FONT);
  EXPECT_EQ(StagedFileTransaction::publish(FONT, FONT_TEMP, FONT_BACKUP, fontValidator),
            StagedFileTransaction::Status::IoError);
  EXPECT_TRUE(Storage.exists(FONT));
  EXPECT_EQ(Storage.file(FONT).back(), 1);
}

TEST_F(AtomicPersistenceTest, InterruptedFontPublishRecoversBackup) {
  Storage.setFile(FONT_BACKUP, fontBytes(1));
  EXPECT_EQ(StagedFileTransaction::recover(FONT, FONT_BACKUP, fontValidator),
            StagedFileTransaction::Status::Recovered);
  EXPECT_TRUE(Storage.exists(FONT));
  EXPECT_EQ(Storage.file(FONT).back(), 1);
}

TEST_F(AtomicPersistenceTest, ClockCalendarConvertsRtcUtcWithoutLocalOffset) {
  const ClockCalendar::DateTime rtc{2026, 7, 22, 14, 35, 10, 3};
  time_t epoch = 0;
  ASSERT_TRUE(ClockCalendar::toEpoch(rtc, epoch));
  ClockCalendar::DateTime restored;
  ASSERT_TRUE(ClockCalendar::fromEpoch(epoch, restored));
  EXPECT_EQ(restored.year, 2026);
  EXPECT_EQ(restored.month, 7);
  EXPECT_EQ(restored.day, 22);
  EXPECT_EQ(restored.hour, 14);
  EXPECT_EQ(restored.minute, 35);
}

TEST_F(AtomicPersistenceTest, ClockCalendarRejectsInvalidRtcAndHandlesMidnight) {
  time_t epoch = 0;
  EXPECT_FALSE(ClockCalendar::toEpoch({2025, 2, 29, 0, 0, 0, 0}, epoch));
  ASSERT_TRUE(ClockCalendar::toEpoch({2024, 2, 29, 23, 59, 59, 4}, epoch));
  ClockCalendar::DateTime next;
  ASSERT_TRUE(ClockCalendar::fromEpoch(epoch + 1, next));
  EXPECT_EQ(next.year, 2024);
  EXPECT_EQ(next.month, 3);
  EXPECT_EQ(next.day, 1);
  EXPECT_EQ(next.hour, 0);
}

TEST_F(AtomicPersistenceTest, NtpPolicyDoesNotTrustPersistedFlagWhenCurrentClockIsInvalid) {
  EXPECT_TRUE(ClockSyncPolicy::shouldSyncFromNetwork(true, false));
  EXPECT_TRUE(ClockSyncPolicy::shouldSyncFromNetwork(false, true));
  EXPECT_FALSE(ClockSyncPolicy::shouldSyncFromNetwork(true, true));
}

TEST_F(AtomicPersistenceTest, TiltSensorOnlyStaysAwakeForVisibleReader) {
  EXPECT_TRUE(TiltLifecyclePolicy::shouldBeAwake(1, true));
  EXPECT_TRUE(TiltLifecyclePolicy::shouldBeAwake(2, true));
  EXPECT_FALSE(TiltLifecyclePolicy::shouldBeAwake(1, false));
  EXPECT_FALSE(TiltLifecyclePolicy::shouldBeAwake(0, true));
}

}  // namespace
