#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

#include "lib/JsonParser/ReleaseJsonParser.h"

static int testsPassed = 0;
static int testsFailed = 0;

#define ASSERT_EQ(a, b)                                                           \
  do {                                                                            \
    auto _a = (a);                                                                \
    auto _b = (b);                                                                \
    if (_a != _b) {                                                               \
      fprintf(stderr, "  FAIL: %s:%d: %s != expected\n", __FILE__, __LINE__, #a); \
      testsFailed++;                                                              \
      return;                                                                     \
    }                                                                             \
  } while (0)

#define ASSERT_STREQ(a, b)                                                              \
  do {                                                                                  \
    const char* _a = (a);                                                               \
    const char* _b = (b);                                                               \
    if (strcmp(_a, _b) != 0) {                                                          \
      fprintf(stderr, "  FAIL: %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, _a, _b); \
      testsFailed++;                                                                    \
      return;                                                                           \
    }                                                                                   \
  } while (0)

#define ASSERT_TRUE(cond)                                                \
  do {                                                                   \
    if (!(cond)) {                                                       \
      fprintf(stderr, "  FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      testsFailed++;                                                     \
      return;                                                            \
    }                                                                    \
  } while (0)

#define PASS() testsPassed++

// ============================================================================
// Realistic GitHub release JSON payloads
// ============================================================================

static const char* kRealisticPretty = R"({
  "url": "https://api.github.com/repos/crosspoint-reader/crosspoint-reader/releases/12345",
  "assets_url": "https://api.github.com/repos/crosspoint-reader/crosspoint-reader/releases/12345/assets",
  "upload_url": "https://uploads.github.com/repos/crosspoint-reader/crosspoint-reader/releases/12345/assets{?name,label}",
  "html_url": "https://github.com/crosspoint-reader/crosspoint-reader/releases/tag/v2.4.1",
  "id": 12345,
  "author": {
    "login": "releasebot",
    "id": 99887766,
    "node_id": "MDQ6VXNlcjk5ODg3NzY2",
    "avatar_url": "https://avatars.githubusercontent.com/u/99887766?v=4",
    "url": "https://api.github.com/users/releasebot",
    "type": "User",
    "site_admin": false
  },
  "node_id": "RE_kwDOAbCdEf4AADBN",
  "tag_name": "v2.4.1",
  "target_commitish": "main",
  "name": "CrossPoint Reader v2.4.1",
  "draft": false,
  "prerelease": false,
  "created_at": "2026-04-28T10:00:00Z",
  "published_at": "2026-04-28T10:30:00Z",
  "assets": [
    {
      "url": "https://api.github.com/repos/crosspoint-reader/crosspoint-reader/releases/assets/100001",
      "id": 100001,
      "node_id": "RA_kwDOAbCdEf4AAGHR",
      "name": "crosspoint-reader-v2.4.1-source.zip",
      "label": null,
      "uploader": {
        "login": "releasebot",
        "id": 99887766,
        "node_id": "MDQ6VXNlcjk5ODg3NzY2",
        "type": "User"
      },
      "content_type": "application/zip",
      "state": "uploaded",
      "size": 2048576,
      "download_count": 42,
      "created_at": "2026-04-28T10:15:00Z",
      "updated_at": "2026-04-28T10:15:30Z",
      "browser_download_url": "https://github.com/crosspoint-reader/crosspoint-reader/releases/download/v2.4.1/crosspoint-reader-v2.4.1-source.zip"
    },
    {
      "url": "https://api.github.com/repos/crosspoint-reader/crosspoint-reader/releases/assets/100002",
      "id": 100002,
      "node_id": "RA_kwDOAbCdEf4AAGHS",
      "name": "firmware.bin",
      "label": "ESP32-C3 Firmware",
      "uploader": {
        "login": "releasebot",
        "id": 99887766,
        "node_id": "MDQ6VXNlcjk5ODg3NzY2",
        "type": "User"
      },
      "content_type": "application/octet-stream",
      "state": "uploaded",
      "size": 1572864,
      "download_count": 187,
      "created_at": "2026-04-28T10:16:00Z",
      "updated_at": "2026-04-28T10:16:45Z",
      "browser_download_url": "https://github.com/crosspoint-reader/crosspoint-reader/releases/download/v2.4.1/firmware.bin"
    },
    {
      "url": "https://api.github.com/repos/crosspoint-reader/crosspoint-reader/releases/assets/100003",
      "id": 100003,
      "node_id": "RA_kwDOAbCdEf4AAGHR",
      "name": "checksums.sha256",
      "label": null,
      "uploader": {
        "login": "releasebot",
        "id": 99887766,
        "node_id": "MDQ6VXNlcjk5ODg3NzY2",
        "type": "User"
      },
      "content_type": "text/plain",
      "state": "uploaded",
      "size": 192,
      "download_count": 15,
      "created_at": "2026-04-28T10:17:00Z",
      "updated_at": "2026-04-28T10:17:10Z",
      "browser_download_url": "https://github.com/crosspoint-reader/crosspoint-reader/releases/download/v2.4.1/checksums.sha256"
    }
  ],
  "tarball_url": "https://api.github.com/repos/crosspoint-reader/crosspoint-reader/tarball/v2.4.1",
  "zipball_url": "https://api.github.com/repos/crosspoint-reader/crosspoint-reader/zipball/v2.4.1",
  "body": "## What's Changed\n\n* Fixed orientation crash (#123)\n* Improved EPUB rendering performance\n* Added Serbian translation\n\n**Full Changelog**: https://github.com/crosspoint-reader/crosspoint-reader/compare/v2.4.0...v2.4.1",
  "reactions": {
    "url": "https://api.github.com/repos/crosspoint-reader/crosspoint-reader/releases/12345/reactions",
    "total_count": 5,
    "+1": 3,
    "-1": 0,
    "laugh": 1,
    "hooray": 1,
    "confused": 0,
    "heart": 0,
    "rocket": 0,
    "eyes": 0
  }
})";

static const char* kRealisticMinified =
    R"({"url":"https://api.github.com/repos/crosspoint-reader/crosspoint-reader/releases/12345","assets_url":"https://api.github.com/repos/crosspoint-reader/crosspoint-reader/releases/12345/assets","id":12345,"author":{"login":"releasebot","id":99887766,"node_id":"MDQ6VXNlcjk5ODg3NzY2","type":"User","site_admin":false},"tag_name":"v2.4.1","target_commitish":"main","name":"CrossPoint Reader v2.4.1","draft":false,"prerelease":false,"assets":[{"url":"https://api.github.com/repos/crosspoint-reader/crosspoint-reader/releases/assets/100001","id":100001,"name":"crosspoint-reader-v2.4.1-source.zip","uploader":{"login":"releasebot","id":99887766},"content_type":"application/zip","state":"uploaded","size":2048576,"download_count":42,"browser_download_url":"https://github.com/crosspoint-reader/crosspoint-reader/releases/download/v2.4.1/crosspoint-reader-v2.4.1-source.zip"},{"url":"https://api.github.com/repos/crosspoint-reader/crosspoint-reader/releases/assets/100002","id":100002,"name":"firmware.bin","uploader":{"login":"releasebot","id":99887766},"content_type":"application/octet-stream","state":"uploaded","size":1572864,"download_count":187,"browser_download_url":"https://github.com/crosspoint-reader/crosspoint-reader/releases/download/v2.4.1/firmware.bin"},{"url":"https://api.github.com/repos/crosspoint-reader/crosspoint-reader/releases/assets/100003","id":100003,"name":"checksums.sha256","uploader":{"login":"releasebot","id":99887766},"content_type":"text/plain","state":"uploaded","size":192,"download_count":15,"browser_download_url":"https://github.com/crosspoint-reader/crosspoint-reader/releases/download/v2.4.1/checksums.sha256"}],"body":"## What's Changed\n\n* Fixed orientation crash","reactions":{"url":"https://api.github.com/repos/crosspoint-reader/crosspoint-reader/releases/12345/reactions","total_count":5,"+1":3}})";

// Helper: feed JSON in fixed-size chunks
static void feedChunked(ReleaseJsonParser& p, const char* json, size_t chunkSize) {
  size_t len = strlen(json);
  for (size_t off = 0; off < len; off += chunkSize) {
    size_t n = len - off < chunkSize ? len - off : chunkSize;
    p.feed(json + off, n);
  }
}

// ============================================================================
// Tests
// ============================================================================

void testRealisticPrettyPrinted() {
  printf("testRealisticPrettyPrinted...\n");

  ReleaseJsonParser p;
  p.feed(kRealisticPretty, strlen(kRealisticPretty));

  ASSERT_TRUE(p.foundTag());
  ASSERT_TRUE(p.foundFirmware());
  ASSERT_STREQ(p.getTagName(), "v2.4.1");
  ASSERT_STREQ(p.getFirmwareUrl(),
               "https://github.com/crosspoint-reader/crosspoint-reader/releases/download/v2.4.1/firmware.bin");
  ASSERT_EQ(p.getFirmwareSize(), 1572864u);

  printf("  passed\n");
  PASS();
}

void testRealisticMinified() {
  printf("testRealisticMinified...\n");

  ReleaseJsonParser p;
  p.feed(kRealisticMinified, strlen(kRealisticMinified));

  ASSERT_TRUE(p.foundTag());
  ASSERT_TRUE(p.foundFirmware());
  ASSERT_STREQ(p.getTagName(), "v2.4.1");
  ASSERT_STREQ(p.getFirmwareUrl(),
               "https://github.com/crosspoint-reader/crosspoint-reader/releases/download/v2.4.1/firmware.bin");
  ASSERT_EQ(p.getFirmwareSize(), 1572864u);

  printf("  passed\n");
  PASS();
}

void testPrettyAndMinifiedAgree() {
  printf("testPrettyAndMinifiedAgree...\n");

  ReleaseJsonParser pretty;
  pretty.feed(kRealisticPretty, strlen(kRealisticPretty));

  ReleaseJsonParser minified;
  minified.feed(kRealisticMinified, strlen(kRealisticMinified));

  ASSERT_STREQ(pretty.getTagName(), minified.getTagName());
  ASSERT_STREQ(pretty.getFirmwareUrl(), minified.getFirmwareUrl());
  ASSERT_EQ(pretty.getFirmwareSize(), minified.getFirmwareSize());

  printf("  passed\n");
  PASS();
}

void testFirmwareNotFirstAsset() {
  printf("testFirmwareNotFirstAsset...\n");

  // firmware.bin is the third of four assets
  const char* json = R"({
      "tag_name": "v1.0.0",
      "assets": [
        {"name": "source.tar.gz", "browser_download_url": "https://example.com/src.tar.gz", "size": 500000},
        {"name": "docs.pdf", "browser_download_url": "https://example.com/docs.pdf", "size": 120000},
        {"name": "firmware.bin", "browser_download_url": "https://example.com/firmware.bin", "size": 987654},
        {"name": "checksums.txt", "browser_download_url": "https://example.com/checksums.txt", "size": 256}
      ]
    })";

  ReleaseJsonParser p;
  p.feed(json, strlen(json));

  ASSERT_TRUE(p.foundTag());
  ASSERT_TRUE(p.foundFirmware());
  ASSERT_STREQ(p.getTagName(), "v1.0.0");
  ASSERT_STREQ(p.getFirmwareUrl(), "https://example.com/firmware.bin");
  ASSERT_EQ(p.getFirmwareSize(), 987654u);

  printf("  passed\n");
  PASS();
}

void testFieldOrderUrlBeforeName() {
  printf("testFieldOrderUrlBeforeName...\n");

  const char* json = R"({
      "tag_name": "v3.0",
      "assets": [{
        "browser_download_url": "https://example.com/fw.bin",
        "name": "firmware.bin",
        "size": 2222
      }]
    })";

  ReleaseJsonParser p;
  p.feed(json, strlen(json));

  ASSERT_TRUE(p.foundFirmware());
  ASSERT_STREQ(p.getFirmwareUrl(), "https://example.com/fw.bin");
  ASSERT_EQ(p.getFirmwareSize(), 2222u);

  printf("  passed\n");
  PASS();
}

void testFieldOrderSizeBeforeUrl() {
  printf("testFieldOrderSizeBeforeUrl...\n");

  const char* json = R"({
      "tag_name": "v3.1",
      "assets": [{
        "size": 3333,
        "browser_download_url": "https://example.com/fw2.bin",
        "name": "firmware.bin"
      }]
    })";

  ReleaseJsonParser p;
  p.feed(json, strlen(json));

  ASSERT_TRUE(p.foundFirmware());
  ASSERT_STREQ(p.getFirmwareUrl(), "https://example.com/fw2.bin");
  ASSERT_EQ(p.getFirmwareSize(), 3333u);

  printf("  passed\n");
  PASS();
}

void testFieldOrderNameFirst() {
  printf("testFieldOrderNameFirst...\n");

  const char* json = R"({
      "tag_name": "v3.2",
      "assets": [{
        "name": "firmware.bin",
        "size": 4444,
        "browser_download_url": "https://example.com/fw3.bin"
      }]
    })";

  ReleaseJsonParser p;
  p.feed(json, strlen(json));

  ASSERT_TRUE(p.foundFirmware());
  ASSERT_STREQ(p.getFirmwareUrl(), "https://example.com/fw3.bin");
  ASSERT_EQ(p.getFirmwareSize(), 4444u);

  printf("  passed\n");
  PASS();
}

void testAssetsBeforeTagName() {
  printf("testAssetsBeforeTagName...\n");

  // tag_name appears after assets in the JSON
  const char* json = R"({
      "name": "Release",
      "assets": [{
        "name": "firmware.bin",
        "browser_download_url": "https://example.com/fw.bin",
        "size": 5555
      }],
      "tag_name": "v4.0"
    })";

  ReleaseJsonParser p;
  p.feed(json, strlen(json));

  ASSERT_TRUE(p.foundTag());
  ASSERT_TRUE(p.foundFirmware());
  ASSERT_STREQ(p.getTagName(), "v4.0");
  ASSERT_STREQ(p.getFirmwareUrl(), "https://example.com/fw.bin");
  ASSERT_EQ(p.getFirmwareSize(), 5555u);

  printf("  passed\n");
  PASS();
}

void testChunkedFeedingRealisticSmallChunks() {
  printf("testChunkedFeedingRealisticSmallChunks...\n");

  // Simulate HTTP chunked transfer with small chunks (64 bytes)
  ReleaseJsonParser p;
  feedChunked(p, kRealisticPretty, 64);

  ASSERT_TRUE(p.foundTag());
  ASSERT_TRUE(p.foundFirmware());
  ASSERT_STREQ(p.getTagName(), "v2.4.1");
  ASSERT_STREQ(p.getFirmwareUrl(),
               "https://github.com/crosspoint-reader/crosspoint-reader/releases/download/v2.4.1/firmware.bin");
  ASSERT_EQ(p.getFirmwareSize(), 1572864u);

  printf("  passed\n");
  PASS();
}

void testChunkedFeedingByteByByte() {
  printf("testChunkedFeedingByteByByte...\n");

  ReleaseJsonParser p;
  feedChunked(p, kRealisticMinified, 1);

  ASSERT_TRUE(p.foundTag());
  ASSERT_TRUE(p.foundFirmware());
  ASSERT_STREQ(p.getTagName(), "v2.4.1");
  ASSERT_EQ(p.getFirmwareSize(), 1572864u);

  printf("  passed\n");
  PASS();
}

void testChunkedFeedingVariousChunkSizes() {
  printf("testChunkedFeedingVariousChunkSizes...\n");

  for (size_t chunkSize : {3, 7, 13, 31, 97, 128, 256, 512, 1024}) {
    ReleaseJsonParser p;
    feedChunked(p, kRealisticPretty, chunkSize);

    ASSERT_TRUE(p.foundTag());
    ASSERT_TRUE(p.foundFirmware());
    ASSERT_STREQ(p.getTagName(), "v2.4.1");
    ASSERT_EQ(p.getFirmwareSize(), 1572864u);
  }

  printf("  passed (9 chunk sizes)\n");
  PASS();
}

void testMissingTagName() {
  printf("testMissingTagName...\n");

  const char* json = R"({
      "name": "Some Release",
      "draft": false,
      "assets": [{
        "name": "firmware.bin",
        "browser_download_url": "https://example.com/fw.bin",
        "size": 1000
      }]
    })";

  ReleaseJsonParser p;
  p.feed(json, strlen(json));

  ASSERT_TRUE(!p.foundTag());
  ASSERT_TRUE(p.foundFirmware());
  ASSERT_STREQ(p.getTagName(), "");

  printf("  passed\n");
  PASS();
}

void testMissingFirmwareBinAsset() {
  printf("testMissingFirmwareBinAsset...\n");

  const char* json = R"({
      "tag_name": "v1.0.0",
      "assets": [
        {"name": "source.zip", "browser_download_url": "https://example.com/src.zip", "size": 1000},
        {"name": "docs.tar.gz", "browser_download_url": "https://example.com/docs.tar.gz", "size": 2000}
      ]
    })";

  ReleaseJsonParser p;
  p.feed(json, strlen(json));

  ASSERT_TRUE(p.foundTag());
  ASSERT_TRUE(!p.foundFirmware());
  ASSERT_STREQ(p.getTagName(), "v1.0.0");
  ASSERT_STREQ(p.getFirmwareUrl(), "");
  ASSERT_EQ(p.getFirmwareSize(), 0u);

  printf("  passed\n");
  PASS();
}

void testEmptyAssetsArray() {
  printf("testEmptyAssetsArray...\n");

  const char* json = R"({"tag_name": "v1.0.0", "assets": []})";

  ReleaseJsonParser p;
  p.feed(json, strlen(json));

  ASSERT_TRUE(p.foundTag());
  ASSERT_TRUE(!p.foundFirmware());

  printf("  passed\n");
  PASS();
}

void testNoAssetsKey() {
  printf("testNoAssetsKey...\n");

  const char* json = R"({"tag_name": "v1.0.0", "name": "Release"})";

  ReleaseJsonParser p;
  p.feed(json, strlen(json));

  ASSERT_TRUE(p.foundTag());
  ASSERT_TRUE(!p.foundFirmware());

  printf("  passed\n");
  PASS();
}

void testTruncatedBeforeTagValue() {
  printf("testTruncatedBeforeTagValue...\n");

  const char* json = R"({"tag_name": )";

  ReleaseJsonParser p;
  p.feed(json, strlen(json));

  ASSERT_TRUE(!p.foundTag());
  ASSERT_TRUE(!p.foundFirmware());

  printf("  passed\n");
  PASS();
}

void testTruncatedInsideTagValue() {
  printf("testTruncatedInsideTagValue...\n");

  const char* json = R"({"tag_name": "v2.4)";

  ReleaseJsonParser p;
  p.feed(json, strlen(json));

  ASSERT_TRUE(!p.foundTag());

  printf("  passed\n");
  PASS();
}

void testTruncatedInsideAssetsArray() {
  printf("testTruncatedInsideAssetsArray...\n");

  const char* json = R"({"tag_name": "v2.4.1", "assets": [{"name": "firm)";

  ReleaseJsonParser p;
  p.feed(json, strlen(json));

  ASSERT_TRUE(p.foundTag());
  ASSERT_STREQ(p.getTagName(), "v2.4.1");
  ASSERT_TRUE(!p.foundFirmware());

  printf("  passed\n");
  PASS();
}

void testTruncatedAfterFirmwareName() {
  printf("testTruncatedAfterFirmwareName...\n");

  // Found the name but connection dropped before URL/size
  const char* json = R"({"tag_name":"v1.0","assets":[{"name":"firmware.bin","browser_dow)";

  ReleaseJsonParser p;
  p.feed(json, strlen(json));

  ASSERT_TRUE(p.foundTag());
  ASSERT_TRUE(!p.foundFirmware());

  printf("  passed\n");
  PASS();
}

void testTruncatedRealisticJson() {
  printf("testTruncatedRealisticJson...\n");

  // Truncate the realistic JSON at various points; none should crash
  std::string full(kRealisticPretty);
  for (size_t cutPoint : {10u, 50u, 100u, 200u, 500u, 1000u, 1500u, 2000u}) {
    if (cutPoint >= full.size()) continue;

    ReleaseJsonParser p;
    p.feed(full.c_str(), cutPoint);
    // Just verify no crash; results depend on where we cut
    (void)p.foundTag();
    (void)p.foundFirmware();
  }

  printf("  passed (no crashes on truncated realistic JSON)\n");
  PASS();
}

void testNestedObjectsInAsset() {
  printf("testNestedObjectsInAsset...\n");

  // Asset with deeply nested "uploader" object -- should not confuse depth tracking
  const char* json = R"({
      "tag_name": "v5.0",
      "assets": [{
        "name": "firmware.bin",
        "uploader": {
          "login": "bot",
          "id": 42,
          "permissions": {"admin": false, "push": true, "pull": true}
        },
        "browser_download_url": "https://example.com/fw5.bin",
        "size": 8888
      }]
    })";

  ReleaseJsonParser p;
  p.feed(json, strlen(json));

  ASSERT_TRUE(p.foundFirmware());
  ASSERT_STREQ(p.getFirmwareUrl(), "https://example.com/fw5.bin");
  ASSERT_EQ(p.getFirmwareSize(), 8888u);

  printf("  passed\n");
  PASS();
}

void testNestedObjectsAtTopLevel() {
  printf("testNestedObjectsAtTopLevel...\n");

  // Multiple nested objects at the top level before/after tag_name and assets
  const char* json = R"({
      "author": {"login": "dev", "id": 1, "nested": {"deep": true}},
      "tag_name": "v6.0",
      "reactions": {"url": "https://reactions", "total_count": 0, "+1": 0},
      "assets": [{"name": "firmware.bin", "browser_download_url": "https://fw6", "size": 1111}],
      "mentions_count": 3
    })";

  ReleaseJsonParser p;
  p.feed(json, strlen(json));

  ASSERT_TRUE(p.foundTag());
  ASSERT_TRUE(p.foundFirmware());
  ASSERT_STREQ(p.getTagName(), "v6.0");
  ASSERT_EQ(p.getFirmwareSize(), 1111u);

  printf("  passed\n");
  PASS();
}

void testArraysAtTopLevel() {
  printf("testArraysAtTopLevel...\n");

  // A non-assets array at the top level should not interfere
  const char* json = R"({
      "tag_name": "v7.0",
      "labels": ["release", "stable"],
      "assets": [{"name": "firmware.bin", "browser_download_url": "https://fw7", "size": 7070}]
    })";

  ReleaseJsonParser p;
  p.feed(json, strlen(json));

  ASSERT_TRUE(p.foundTag());
  ASSERT_TRUE(p.foundFirmware());
  ASSERT_STREQ(p.getTagName(), "v7.0");
  ASSERT_EQ(p.getFirmwareSize(), 7070u);

  printf("  passed\n");
  PASS();
}

void testResetAndReuse() {
  printf("testResetAndReuse...\n");

  ReleaseJsonParser p;

  const char* json1 =
      R"({"tag_name":"v1.0","assets":[{"name":"firmware.bin","browser_download_url":"https://a","size":1}]})";
  p.feed(json1, strlen(json1));
  ASSERT_TRUE(p.foundTag());
  ASSERT_STREQ(p.getTagName(), "v1.0");
  ASSERT_STREQ(p.getFirmwareUrl(), "https://a");
  ASSERT_EQ(p.getFirmwareSize(), 1u);

  p.reset();

  // Second document with different values
  const char* json2 =
      R"({"tag_name":"v2.0","assets":[{"name":"firmware.bin","browser_download_url":"https://b","size":2}]})";
  p.feed(json2, strlen(json2));
  ASSERT_TRUE(p.foundTag());
  ASSERT_STREQ(p.getTagName(), "v2.0");
  ASSERT_STREQ(p.getFirmwareUrl(), "https://b");
  ASSERT_EQ(p.getFirmwareSize(), 2u);

  printf("  passed\n");
  PASS();
}

void testResetClearsState() {
  printf("testResetClearsState...\n");

  ReleaseJsonParser p;

  const char* json =
      R"({"tag_name":"v1.0","assets":[{"name":"firmware.bin","browser_download_url":"https://a","size":100}]})";
  p.feed(json, strlen(json));
  ASSERT_TRUE(p.foundTag());
  ASSERT_TRUE(p.foundFirmware());

  p.reset();

  ASSERT_TRUE(!p.foundTag());
  ASSERT_TRUE(!p.foundFirmware());
  ASSERT_STREQ(p.getTagName(), "");
  ASSERT_STREQ(p.getFirmwareUrl(), "");
  ASSERT_EQ(p.getFirmwareSize(), 0u);

  printf("  passed\n");
  PASS();
}

void testPartialAssetNameMatch() {
  printf("testPartialAssetNameMatch...\n");

  // "firmware.bin.bak" should NOT match "firmware.bin"
  const char* json = R"({
      "tag_name": "v1.0",
      "assets": [
        {"name": "firmware.bin.bak", "browser_download_url": "https://bak", "size": 100},
        {"name": "firmware.bin.old", "browser_download_url": "https://old", "size": 200}
      ]
    })";

  ReleaseJsonParser p;
  p.feed(json, strlen(json));

  ASSERT_TRUE(p.foundTag());
  ASSERT_TRUE(!p.foundFirmware());

  printf("  passed\n");
  PASS();
}

void testFirmwareBinExactMatch() {
  printf("testFirmwareBinExactMatch...\n");

  // Only exact "firmware.bin" matches, not similar names
  const char* json = R"({
      "tag_name": "v1.0",
      "assets": [
        {"name": "FIRMWARE.BIN", "browser_download_url": "https://upper", "size": 100},
        {"name": "firmware.bin", "browser_download_url": "https://exact", "size": 200},
        {"name": "firmware.bin2", "browser_download_url": "https://suffix", "size": 300}
      ]
    })";

  ReleaseJsonParser p;
  p.feed(json, strlen(json));

  ASSERT_TRUE(p.foundFirmware());
  ASSERT_STREQ(p.getFirmwareUrl(), "https://exact");
  ASSERT_EQ(p.getFirmwareSize(), 200u);

  printf("  passed\n");
  PASS();
}

void testLargeSize() {
  printf("testLargeSize...\n");

  // 16MB firmware (maximum flash size)
  const char* json =
      R"({"tag_name":"v1.0","assets":[{"name":"firmware.bin","browser_download_url":"https://fw","size":16777216}]})";

  ReleaseJsonParser p;
  p.feed(json, strlen(json));

  ASSERT_EQ(p.getFirmwareSize(), 16777216u);

  printf("  passed\n");
  PASS();
}

void testSizeZero() {
  printf("testSizeZero...\n");

  const char* json =
      R"({"tag_name":"v1.0","assets":[{"name":"firmware.bin","browser_download_url":"https://fw","size":0}]})";

  ReleaseJsonParser p;
  p.feed(json, strlen(json));

  ASSERT_TRUE(p.foundFirmware());
  ASSERT_EQ(p.getFirmwareSize(), 0u);

  printf("  passed\n");
  PASS();
}

void testMinimalValidJson() {
  printf("testMinimalValidJson...\n");

  const char* json = R"({"tag_name":"v0","assets":[{"name":"firmware.bin","browser_download_url":"u","size":1}]})";

  ReleaseJsonParser p;
  p.feed(json, strlen(json));

  ASSERT_TRUE(p.foundTag());
  ASSERT_TRUE(p.foundFirmware());
  ASSERT_STREQ(p.getTagName(), "v0");
  ASSERT_STREQ(p.getFirmwareUrl(), "u");
  ASSERT_EQ(p.getFirmwareSize(), 1u);

  printf("  passed\n");
  PASS();
}

void testChunkedRealisticEveryBoundary() {
  printf("testChunkedRealisticEveryBoundary...\n");

  // Two-chunk split at every byte boundary on a compact JSON
  const char* json =
      R"({"tag_name":"v2.0","assets":[{"name":"firmware.bin","browser_download_url":"https://example.com/fw","size":9999}]})";
  size_t len = strlen(json);

  for (size_t split = 0; split <= len; ++split) {
    ReleaseJsonParser p;
    if (split > 0) p.feed(json, split);
    if (split < len) p.feed(json + split, len - split);

    ASSERT_TRUE(p.foundTag());
    ASSERT_TRUE(p.foundFirmware());
    ASSERT_STREQ(p.getTagName(), "v2.0");
    ASSERT_STREQ(p.getFirmwareUrl(), "https://example.com/fw");
    ASSERT_EQ(p.getFirmwareSize(), 9999u);
  }

  printf("  passed (all %zu split points)\n", len + 1);
  PASS();
}

// ============================================================================

int main() {
  printf("=== ReleaseJsonParser Tests ===\n\n");

  testRealisticPrettyPrinted();
  testRealisticMinified();
  testPrettyAndMinifiedAgree();
  testFirmwareNotFirstAsset();
  testFieldOrderUrlBeforeName();
  testFieldOrderSizeBeforeUrl();
  testFieldOrderNameFirst();
  testAssetsBeforeTagName();
  testChunkedFeedingRealisticSmallChunks();
  testChunkedFeedingByteByByte();
  testChunkedFeedingVariousChunkSizes();
  testMissingTagName();
  testMissingFirmwareBinAsset();
  testEmptyAssetsArray();
  testNoAssetsKey();
  testTruncatedBeforeTagValue();
  testTruncatedInsideTagValue();
  testTruncatedInsideAssetsArray();
  testTruncatedAfterFirmwareName();
  testTruncatedRealisticJson();
  testNestedObjectsInAsset();
  testNestedObjectsAtTopLevel();
  testArraysAtTopLevel();
  testResetAndReuse();
  testResetClearsState();
  testPartialAssetNameMatch();
  testFirmwareBinExactMatch();
  testLargeSize();
  testSizeZero();
  testMinimalValidJson();
  testChunkedRealisticEveryBoundary();

  printf("\n=== Results: %d passed, %d failed ===\n", testsPassed, testsFailed);
  return testsFailed > 0 ? 1 : 0;
}
