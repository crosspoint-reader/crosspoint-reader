#include <gtest/gtest.h>

#include "lib/AppStore/AppStoreManifestMetaJson.h"

TEST(AppStoreManifestMetaTest, ParsesMetaJson) {
  AppStoreManifestMetaData meta;
  ASSERT_TRUE(AppStoreManifestMetaJson::parse(R"({"fetched_at":"2026-07-03T19:30:00Z","app_count":14})", meta));
  EXPECT_EQ(meta.fetchedAt, "2026-07-03T19:30:00Z");
  EXPECT_EQ(meta.appCount, 14u);
}

TEST(AppStoreManifestMetaTest, RejectsMissingFetchedAt) {
  AppStoreManifestMetaData meta;
  EXPECT_FALSE(AppStoreManifestMetaJson::parse(R"({"app_count":14})", meta));
}

TEST(AppStoreManifestMetaTest, RoundTripSerialize) {
  AppStoreManifestMetaData meta;
  meta.fetchedAt = "2026-07-03T19:30:00Z";
  meta.appCount = 9;

  const std::string json = AppStoreManifestMetaJson::serialize(meta);
  AppStoreManifestMetaData parsed;
  ASSERT_TRUE(AppStoreManifestMetaJson::parse(json.c_str(), parsed));
  EXPECT_EQ(parsed.fetchedAt, meta.fetchedAt);
  EXPECT_EQ(parsed.appCount, meta.appCount);
}
