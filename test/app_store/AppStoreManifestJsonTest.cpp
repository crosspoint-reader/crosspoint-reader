#include <gtest/gtest.h>

#include <ArduinoJson.h>

#include "lib/AppStore/AppStoreManifestData.h"
#include "lib/AppStore/AppStoreManifestJson.h"

TEST(AppStoreManifestJsonTest, ParsesBuiltinManifestHelloFixture) {
  static constexpr char kJson[] = R"({
    "version": 1,
    "apps": [
      {
        "id": "hello",
        "name": "Hello App",
        "version": "1.0.0",
        "description": "Smoke-test sample for the Lua runtime",
        "min_api_version": "1.0"
      }
    ]
  })";

  std::vector<AppCatalogEntry> entries;
  uint8_t version = 0;
  ASSERT_TRUE(AppStoreManifestJson::parse(kJson, entries, version));
  EXPECT_EQ(version, 1);
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].id, "hello");
  EXPECT_EQ(entries[0].name, "Hello App");
  EXPECT_EQ(entries[0].version, "1.0.0");
  EXPECT_EQ(entries[0].minApiVersion, "1.0");
}

TEST(AppStoreManifestJsonTest, ParsesBuiltinManifestData) {
  std::vector<AppCatalogEntry> entries;
  uint8_t version = 0;
  ASSERT_TRUE(AppStoreManifestJson::parse(AppStoreManifestData::kBuiltinJson, entries, version));
  EXPECT_EQ(version, 1);
  ASSERT_GE(entries.size(), 1u);
}

TEST(AppStoreManifestJsonTest, SkipsIncompleteEntries) {
  static constexpr char kJson[] = R"({
    "version": 1,
    "apps": [
      {"id": "ok", "name": "OK App", "version": "1.0.0"},
      {"id": "bad", "name": "Missing Version"},
      {"name": "Missing Id", "version": "1.0.0"}
    ]
  })";

  std::vector<AppCatalogEntry> entries;
  uint8_t version = 0;
  ASSERT_TRUE(AppStoreManifestJson::parse(kJson, entries, version));
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].id, "ok");
}
