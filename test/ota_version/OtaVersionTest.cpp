#include <HttpDownloader.h>
#include <OtaTestSupport.h>
#include <OtaUpdater.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

namespace {

void setRelease(const std::string_view version) {
  const std::string assetName = "crosspoint-" + std::string(version) + "-x3-x4.bin";
  ota_test::releaseJson = "{\"tag_name\":\"" + std::string(version) + "\",\"assets\":[{\"name\":\"" + assetName +
                          "\",\"browser_download_url\":"
                          "\"https://example.com/firmware.bin\",\"size\":100}]}";
}

class OtaVersionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ota_test::currentVersion = "1.6.0";
    ota_test::flashBegins = 0;
    ota_test::chunkSize = 7;
  }

  void expectUpdate(const std::string_view latest, const std::string_view current, bool expected) {
    SCOPED_TRACE(std::string(latest) + " vs " + std::string(current));
    ota_test::currentVersion = current;
    setRelease(latest);
    OtaUpdater updater;
    ASSERT_EQ(updater.checkForUpdate(), OtaUpdater::OK);
    EXPECT_EQ(updater.isUpdateNewer(), expected);
  }
};

TEST_F(OtaVersionTest, DoesNotOfferUncheckedUpdate) {
  OtaUpdater updater;
  EXPECT_FALSE(updater.isUpdateNewer());
}

TEST_F(OtaVersionTest, ComparesAllCoreComponents) {
  expectUpdate("2.0.0", "1.9.9", true);
  expectUpdate("1.7.0", "1.6.99", true);
  expectUpdate("1.6.1", "1.6.0", true);
  expectUpdate("1.6.0", "1.6.0", false);
  expectUpdate("1.6.0", "1.6.1", false);
  expectUpdate("1.6.99", "1.7.0", false);
  expectUpdate("1.99.99", "2.0.0", false);
}

TEST_F(OtaVersionTest, AcceptsOptionalVPrefix) {
  expectUpdate("v1.6.1", "1.6.0", true);
  expectUpdate("1.6.1", "v1.6.0", true);
  expectUpdate("v1.6.0", "1.6.0", false);
}

TEST_F(OtaVersionTest, StableReleaseReplacesAnyPrerelease) {
  expectUpdate("1.6.0", "1.6.0-rc.1", true);
  expectUpdate("1.6.0", "1.6.0-beta.1", true);
  expectUpdate("1.6.0", "1.6.0-dev-develop-abc1234", true);
  expectUpdate("1.6.0-rc.1", "1.6.0", false);
}

TEST_F(OtaVersionTest, OrdersPrereleaseWithoutDowngrading) {
  expectUpdate("1.6.0-rc.1", "1.6.0-rc.2", false);
  expectUpdate("1.6.0-rc.10", "1.6.0-rc.2", true);
  expectUpdate("1.6.0-alpha.1", "1.6.0-alpha", true);
  expectUpdate("1.6.0-alpha.beta", "1.6.0-alpha.1", true);
  expectUpdate("1.6.0-beta", "1.6.0-alpha.beta", true);
  expectUpdate("1.6.0-alpha", "1.6.0-alpha.1", false);
  constexpr std::array<std::string_view, 8> ordered = {"1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta",
                                                       "1.0.0-beta",  "1.0.0-beta.2",  "1.0.0-beta.11",
                                                       "1.0.0-rc.1",  "1.0.0"};
  for (size_t index = 1; index < ordered.size(); ++index) {
    expectUpdate(ordered[index], ordered[index - 1], true);
    expectUpdate(ordered[index - 1], ordered[index], false);
  }
}

TEST_F(OtaVersionTest, IgnoresBuildMetadata) {
  expectUpdate("1.6.0+new", "1.6.0+old", false);
  expectUpdate("1.6.0-rc.1+new", "1.6.0-rc.1+old", false);
  expectUpdate("1.6.1+001", "1.6.0+100", true);
}

TEST_F(OtaVersionTest, RejectsMalformedReleaseVersions) {
  for (const auto version :
       {"1.7", "1.7.0garbage", "1.7.0.1", "01.7.0", "1.07.0", "1.7.00", "1.7.0 ", "+1.7.0", "1.7.0-", "1.7.0+",
        "1.7.0-a..b", "1.7.0+a..b", "1.7.0-01", "1.7.0-rc.01", "1.7.0-a/b", "1.7.0+a/b", "1.7.0-dev-feature/a"}) {
    expectUpdate(version, "1.6.0", false);
  }
}

TEST_F(OtaVersionTest, RejectsMalformedCurrentVersions) {
  for (const auto version :
       {"", "invalid", "1.5", "1.5.0garbage", "01.5.0", "1.5.0-01", "1.5.0+", "1.5.0-dev-invalid suffix"}) {
    expectUpdate("1.6.0", version, false);
  }
}

TEST_F(OtaVersionTest, NumericComparisonDoesNotOverflow) {
  expectUpdate("18446744073709551616.0.0", "18446744073709551615.0.0", true);
  expectUpdate("18446744073709551615.0.0", "18446744073709551616.0.0", false);
  expectUpdate("1.6.0-99999999999999999999", "1.6.0-9999999999999999999", true);
}

TEST_F(OtaVersionTest, SupportsGitDevelopmentVersionSuffixes) {
  const std::string current = "1.6.0-dev-feature/" + std::string(80, 'a') + "-abc1234";
  expectUpdate("1.6.0", current, true);
  expectUpdate("1.7.0", current, true);
  expectUpdate("1.5.9", current, false);
  expectUpdate("1.6.0", "1.6.0-dev-unknown-unknown", true);
}

TEST_F(OtaVersionTest, PreservesFullTagAtExistingBufferLimit) {
  const std::string latest = "1.7.0-" + std::string(25, 'a');
  ASSERT_EQ(latest.size(), 31U);
  expectUpdate(latest, "1.6.0", true);
}

TEST_F(OtaVersionTest, RejectsTagInsteadOfComparingTruncatedVersion) {
  const std::string latest = "1.7.0-" + std::string(25, 'a') + "x";
  setRelease(latest);
  for (size_t chunk = 1; chunk <= 16; ++chunk) {
    ota_test::chunkSize = chunk;
    OtaUpdater updater;
    EXPECT_EQ(updater.checkForUpdate(), OtaUpdater::JSON_PARSE_ERROR);
    EXPECT_FALSE(updater.isUpdateNewer());
  }
}

TEST_F(OtaVersionTest, EmbeddedNullTagCannotBecomeValidVersionPrefix) {
  const std::string latest = std::string("1.7.0") + '\0' + "garbage";
  setRelease(latest);
  OtaUpdater updater;
  EXPECT_EQ(updater.checkForUpdate(), OtaUpdater::JSON_PARSE_ERROR);
  EXPECT_FALSE(updater.isUpdateNewer());
}

TEST_F(OtaVersionTest, InvalidVersionCannotStartFlashing) {
  setRelease("1.7.0garbage");
  OtaUpdater updater;
  ASSERT_EQ(updater.checkForUpdate(), OtaUpdater::OK);
  EXPECT_EQ(updater.installUpdate(), OtaUpdater::UPDATE_OLDER_ERROR);
  EXPECT_EQ(ota_test::flashBegins, 0U);
}

}  // namespace

bool HttpDownloader::fetchUrl(const std::string&, const DataCallback& onData, const std::string&, const std::string&) {
  for (size_t offset = 0; offset < ota_test::releaseJson.size(); offset += ota_test::chunkSize) {
    const size_t size = std::min(ota_test::chunkSize, ota_test::releaseJson.size() - offset);
    if (!onData(reinterpret_cast<const uint8_t*>(ota_test::releaseJson.data() + offset), size)) return false;
  }
  return true;
}

namespace firmware_flash {
uint16_t runningPartitionChipId() { return 0xFFFF; }
}  // namespace firmware_flash
