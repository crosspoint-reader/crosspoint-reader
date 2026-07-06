#include <gtest/gtest.h>

#include "lib/AppStore/AppStoreManifest.h"

namespace {

constexpr char kRemoteJson[] = R"({
  "version": 1,
  "apps": [
    {"id": "remote-app", "name": "Remote App", "version": "2.0.0"}
  ]
})";

constexpr char kCacheJson[] = R"({
  "version": 1,
  "apps": [
    {"id": "cached-app", "name": "Cached App", "version": "1.5.0"}
  ]
})";

constexpr char kBuiltinHelloJson[] = R"({
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

}  // namespace

TEST(AppStoreManifestFallbackTest, UsesRemoteWhenFetchSucceeds) {
  AppCatalogResolveInput input;
  input.remoteFetchOk = true;
  input.remoteJson = kRemoteJson;
  input.cacheReadOk = true;
  input.cacheJson = kCacheJson;
  input.builtinJson = kBuiltinHelloJson;

  std::vector<AppCatalogEntry> entries;
  uint8_t version = 0;
  AppCatalogSource source = AppCatalogSource::None;
  ASSERT_TRUE(AppStoreManifest::resolveAndParse(input, entries, version, source));
  EXPECT_EQ(source, AppCatalogSource::Remote);
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].id, "remote-app");
}

TEST(AppStoreManifestFallbackTest, FallsBackToCacheWhenRemoteFails) {
  AppCatalogResolveInput input;
  input.remoteFetchOk = false;
  input.cacheReadOk = true;
  input.cacheJson = kCacheJson;
  input.builtinJson = kBuiltinHelloJson;

  std::vector<AppCatalogEntry> entries;
  uint8_t version = 0;
  AppCatalogSource source = AppCatalogSource::None;
  ASSERT_TRUE(AppStoreManifest::resolveAndParse(input, entries, version, source));
  EXPECT_EQ(source, AppCatalogSource::Cache);
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].id, "cached-app");
}

TEST(AppStoreManifestFallbackTest, FallsBackToBuiltinWhenRemoteAndCacheFail) {
  AppCatalogResolveInput input;
  input.remoteFetchOk = false;
  input.cacheReadOk = false;
  input.builtinJson = kBuiltinHelloJson;

  std::vector<AppCatalogEntry> entries;
  uint8_t version = 0;
  AppCatalogSource source = AppCatalogSource::None;
  ASSERT_TRUE(AppStoreManifest::resolveAndParse(input, entries, version, source));
  EXPECT_EQ(source, AppCatalogSource::Builtin);
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].id, "hello");
}

TEST(AppStoreManifestFallbackTest, FallsBackToBuiltinWhenRemoteJsonInvalid) {
  static constexpr char kBadRemote[] = "{not json";

  AppCatalogResolveInput input;
  input.remoteFetchOk = true;
  input.remoteJson = kBadRemote;
  input.cacheReadOk = false;
  input.builtinJson = kBuiltinHelloJson;

  std::vector<AppCatalogEntry> entries;
  uint8_t version = 0;
  AppCatalogSource source = AppCatalogSource::None;
  ASSERT_TRUE(AppStoreManifest::resolveAndParse(input, entries, version, source));
  EXPECT_EQ(source, AppCatalogSource::Builtin);
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].id, "hello");
}
