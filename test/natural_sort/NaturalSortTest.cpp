#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "FsHelpers/NaturalSort.h"

namespace {

int sign(int v) { return (v > 0) - (v < 0); }

constexpr size_t kFileIndexKeySize = 28;
constexpr size_t kFileIndexChunkEntries = 64;

enum class SortMode { Name, Date, Size, Type };

struct IndexRecord {
  std::array<uint8_t, kFileIndexKeySize> key{};
  uint32_t blobOffset;
  std::string name;
};

// Compare two names via fixed-size zero-padded sort keys, the way the on-SD
// merge sort does: memcmp of the padded keys, naturalCompare as tiebreak.
int keyCompare(const char* a, const char* b, size_t keySize) {
  std::vector<uint8_t> ka(keySize, 0), kb(keySize, 0);
  FsHelpers::naturalSortKey(a, ka.data(), keySize);
  FsHelpers::naturalSortKey(b, kb.data(), keySize);
  return memcmp(ka.data(), kb.data(), keySize);
}

std::array<uint8_t, kFileIndexKeySize> makeIndexKey(const char* name, SortMode mode, uint32_t numeric = 0) {
  std::array<uint8_t, kFileIndexKeySize> key{};
  key[0] = 2;  // file section
  uint8_t* payload = key.data() + 1;
  constexpr size_t payloadSize = kFileIndexKeySize - 1;

  if (mode == SortMode::Type) {
    constexpr size_t extensionKeySize = 8;
    const char* dot = strrchr(name, '.');
    const char* extension = dot && dot != name ? dot + 1 : "";
    FsHelpers::naturalSortKey(extension, payload, extensionKeySize);
    FsHelpers::naturalSortKey(name, payload + extensionKeySize, payloadSize - extensionKeySize);
  } else if (mode == SortMode::Name) {
    FsHelpers::naturalSortKey(name, payload, payloadSize);
  } else {
    payload[0] = static_cast<uint8_t>(numeric >> 24);
    payload[1] = static_cast<uint8_t>(numeric >> 16);
    payload[2] = static_cast<uint8_t>(numeric >> 8);
    payload[3] = static_cast<uint8_t>(numeric);
    FsHelpers::naturalSortKey(name, payload + 4, payloadSize - 4);
  }
  return key;
}

bool indexRecordLess(const IndexRecord& a, const IndexRecord& b) {
  const int keyCmp = memcmp(a.key.data(), b.key.data(), a.key.size());
  if (keyCmp != 0) return keyCmp < 0;
  const int nameCmp = FsHelpers::naturalCompare(a.name.c_str(), b.name.c_str());
  if (nameCmp != 0) return nameCmp < 0;
  return a.blobOffset < b.blobOffset;
}

bool primaryRecordLess(const IndexRecord& a, const IndexRecord& b) {
  const int keyCmp = memcmp(a.key.data(), b.key.data(), a.key.size());
  return keyCmp < 0 || (keyCmp == 0 && a.blobOffset < b.blobOffset);
}

uint32_t scratchBodyBytes(const std::vector<IndexRecord>& records) {
  uint32_t bytes = 0;
  for (const auto& record : records) {
    bytes += 8 + record.name.size();
  }
  return bytes;
}

struct ScratchRun {
  uint32_t bodyBytes;
  std::vector<IndexRecord> records;
};

std::vector<IndexRecord> scratchSortGroup(const std::vector<IndexRecord>& group) {
  std::vector<ScratchRun> runs;
  runs.reserve((group.size() + 1) / 2);
  for (size_t first = 0; first < group.size(); first += 2) {
    std::vector<IndexRecord> pair;
    pair.reserve(2);
    pair.push_back(group[first]);
    if (first + 1 < group.size()) pair.push_back(group[first + 1]);
    std::sort(pair.begin(), pair.end(), indexRecordLess);
    runs.push_back({scratchBodyBytes(pair), std::move(pair)});
  }

  while (runs.size() > 1) {
    std::vector<ScratchRun> mergedRuns;
    mergedRuns.reserve((runs.size() + 1) / 2);
    for (size_t first = 0; first < runs.size(); first += 2) {
      if (first + 1 == runs.size()) {
        mergedRuns.push_back(std::move(runs[first]));
        continue;
      }

      const ScratchRun& a = runs[first];
      const ScratchRun& b = runs[first + 1];
      std::vector<IndexRecord> records;
      records.reserve(a.records.size() + b.records.size());
      std::merge(a.records.begin(), a.records.end(), b.records.begin(), b.records.end(), std::back_inserter(records),
                 indexRecordLess);
      mergedRuns.push_back({a.bodyBytes + b.bodyBytes, std::move(records)});
    }
    runs = std::move(mergedRuns);
  }

  if (runs.empty()) return {};
  EXPECT_EQ(runs[0].bodyBytes, scratchBodyBytes(runs[0].records));
  return std::move(runs[0].records);
}

std::vector<IndexRecord> hybridExternalSort(std::vector<IndexRecord> records) {
  for (size_t first = 0; first < records.size(); first += kFileIndexChunkEntries) {
    const size_t last = std::min(first + kFileIndexChunkEntries, records.size());
    std::sort(records.begin() + first, records.begin() + last, primaryRecordLess);
  }

  std::vector<IndexRecord> merged;
  merged.reserve(records.size());
  for (size_t runSize = kFileIndexChunkEntries; runSize < records.size(); runSize *= 2) {
    merged.clear();
    for (size_t first = 0; first < records.size(); first += runSize * 2) {
      const size_t middle = std::min(first + runSize, records.size());
      const size_t last = std::min(first + runSize * 2, records.size());
      size_t a = first;
      size_t b = middle;
      while (a < middle || b < last) {
        if (b == last || (a < middle && !primaryRecordLess(records[b], records[a]))) {
          merged.push_back(records[a++]);
        } else {
          merged.push_back(records[b++]);
        }
      }
    }
    records.swap(merged);
  }

  for (size_t first = 0; first < records.size();) {
    size_t last = first + 1;
    while (last < records.size() && records[first].key == records[last].key) {
      last++;
    }

    if (last - first <= kFileIndexChunkEntries) {
      std::sort(records.begin() + first, records.begin() + last, indexRecordLess);
    } else {
      const std::vector<IndexRecord> group(records.begin() + first, records.begin() + last);
      auto sorted = scratchSortGroup(group);
      std::move(sorted.begin(), sorted.end(), records.begin() + first);
    }
    first = last;
  }
  return records;
}

// Names listed in strictly increasing naturalCompare order.
const char* const kOrdered[] = {
    "!bang",
    "01 intro",  // digit run: 1
    "1 intro",   // == "01 intro" numerically, kept once below in equality test
    "2 chapter",
    "002 chapter b",  // 2 < 10 despite "002" being longer text
    "10 chapter",
    "Book 1.epub",
    "book 2.epub",  // case-insensitive: Book == book
    "book 10.epub",
    "book 100.epub",
    "book.epub",
    "bookend.epub",
    "zz top",
    "\xC3\xA9tude.epub",  // é (UTF-8) sorts after ASCII byte-wise
};
constexpr size_t kOrderedCount = sizeof(kOrdered) / sizeof(kOrdered[0]);

// Pairs naturalCompare considers equal.
const char* const kEqualPairs[][2] = {
    {"01 intro", "1 intro"},
    {"0a", "00a"},
    {"file 007", "file 7"},
    {"ABC", "abc"},
};

TEST(NaturalCompare, OrderedTable) {
  for (size_t i = 0; i < kOrderedCount; i++) {
    for (size_t j = 0; j < kOrderedCount; j++) {
      if (i == j) continue;
      // Skip the deliberately equal pair in the table
      if (FsHelpers::naturalCompare(kOrdered[i], kOrdered[j]) == 0) continue;
      const int expected = i < j ? -1 : 1;
      EXPECT_EQ(sign(FsHelpers::naturalCompare(kOrdered[i], kOrdered[j])), expected)
          << "\"" << kOrdered[i] << "\" vs \"" << kOrdered[j] << "\"";
    }
  }
}

TEST(NaturalCompare, EqualPairs) {
  for (const auto& pair : kEqualPairs) {
    EXPECT_EQ(FsHelpers::naturalCompare(pair[0], pair[1]), 0) << "\"" << pair[0] << "\" vs \"" << pair[1] << "\"";
    EXPECT_EQ(FsHelpers::naturalCompare(pair[1], pair[0]), 0);
  }
}

TEST(NaturalCompare, SelfEqual) {
  for (const char* name : kOrdered) {
    EXPECT_EQ(FsHelpers::naturalCompare(name, name), 0) << name;
  }
}

// Full-size keys (large enough that nothing truncates) must order exactly
// like naturalCompare for every pair.
TEST(NaturalSortKey, FullKeyMatchesNaturalCompare) {
  constexpr size_t kBigKey = 256;
  for (size_t i = 0; i < kOrderedCount; i++) {
    for (size_t j = 0; j < kOrderedCount; j++) {
      EXPECT_EQ(sign(keyCompare(kOrdered[i], kOrdered[j], kBigKey)),
                sign(FsHelpers::naturalCompare(kOrdered[i], kOrdered[j])))
          << "\"" << kOrdered[i] << "\" vs \"" << kOrdered[j] << "\"";
    }
  }
  for (const auto& pair : kEqualPairs) {
    EXPECT_EQ(keyCompare(pair[0], pair[1], kBigKey), 0) << "\"" << pair[0] << "\" vs \"" << pair[1] << "\"";
  }
}

// Truncated keys may collide (equal prefixes) but must never contradict
// naturalCompare: a non-zero memcmp has to have the same sign.
TEST(NaturalSortKey, TruncatedKeyNeverContradicts) {
  constexpr size_t kSmallKey = 8;
  for (size_t i = 0; i < kOrderedCount; i++) {
    for (size_t j = 0; j < kOrderedCount; j++) {
      const int kc = keyCompare(kOrdered[i], kOrdered[j], kSmallKey);
      if (kc == 0) continue;  // collision: merge sort falls back to naturalCompare
      EXPECT_EQ(sign(kc), sign(FsHelpers::naturalCompare(kOrdered[i], kOrdered[j])))
          << "\"" << kOrdered[i] << "\" vs \"" << kOrdered[j] << "\"";
    }
  }
}

TEST(NaturalSortKey, NeverEmitsZeroBytes) {
  uint8_t key[64];
  for (const char* name : kOrdered) {
    const size_t n = FsHelpers::naturalSortKey(name, key, sizeof(key));
    for (size_t i = 0; i < n; i++) {
      EXPECT_NE(key[i], 0) << name << " at byte " << i;
    }
  }
}

TEST(NaturalSortKey, RespectsCapacity) {
  uint8_t key[4];
  const size_t n = FsHelpers::naturalSortKey("a very long file name 1234567890.epub", key, sizeof(key));
  EXPECT_EQ(n, sizeof(key));
}

class FileIndexCollisionTest : public testing::TestWithParam<std::tuple<SortMode, size_t>> {};

TEST_P(FileIndexCollisionTest, HybridSortRepairsCollisionGroupsByFullName) {
  const auto [mode, count] = GetParam();
  std::vector<IndexRecord> records;
  records.reserve(count);

  for (size_t i = count; i > 0; i--) {
    char name[64];
    snprintf(name, sizeof(name), "My Long Story Title Chapter %04u.txt", static_cast<unsigned>(i));
    const uint32_t numeric = mode == SortMode::Size ? 4096 : 0x5A6B7C8D;
    records.push_back({makeIndexKey(name, mode, numeric), static_cast<uint32_t>(count - i), name});
  }

  for (size_t i = 1; i < records.size(); i++) {
    ASSERT_EQ(records[0].key, records[i].key);
  }

  const auto sorted = hybridExternalSort(std::move(records));
  for (size_t i = 0; i < count; i++) {
    char expected[64];
    snprintf(expected, sizeof(expected), "My Long Story Title Chapter %04u.txt", static_cast<unsigned>(i + 1));
    EXPECT_EQ(sorted[i].name, expected);
  }

  const std::vector<IndexRecord> descending(sorted.rbegin(), sorted.rend());
  for (size_t i = 0; i < count; i++) {
    char expected[64];
    snprintf(expected, sizeof(expected), "My Long Story Title Chapter %04u.txt", static_cast<unsigned>(count - i));
    EXPECT_EQ(descending[i].name, expected);
  }
}

INSTANTIATE_TEST_SUITE_P(AllSortModesAndCollisionSizes, FileIndexCollisionTest,
                         testing::Combine(testing::Values(SortMode::Name, SortMode::Date, SortMode::Size,
                                                          SortMode::Type),
                                          testing::Values<size_t>(64, 65, 512, 2000)));

TEST(FileIndexCollision, MaximumSupportedCardNamesUseScratchTieBreak) {
  constexpr size_t count = 65;
  std::vector<IndexRecord> records;
  records.reserve(count);

  for (size_t i = count; i > 0; i--) {
    char suffix[32];
    snprintf(suffix, sizeof(suffix), " Chapter %04u.txt", static_cast<unsigned>(i));
    const std::string name(255 - strlen(suffix), 'a');
    records.push_back(
        {makeIndexKey((name + suffix).c_str(), SortMode::Name), static_cast<uint32_t>(count - i), name + suffix});
    ASSERT_EQ(records.back().name.size(), 255u);
  }
  for (size_t i = 1; i < records.size(); i++) {
    ASSERT_EQ(records[0].key, records[i].key);
  }

  const auto sorted = hybridExternalSort(std::move(records));
  for (size_t i = 0; i < count; i++) {
    char suffix[32];
    snprintf(suffix, sizeof(suffix), " Chapter %04u.txt", static_cast<unsigned>(i + 1));
    EXPECT_EQ(sorted[i].name.substr(sorted[i].name.size() - strlen(suffix)), suffix);
  }
}

TEST(FileIndexCollision, NaturalEqualNamesUseBlobOffsetTieBreak) {
  constexpr size_t count = 65;
  std::vector<IndexRecord> records;
  records.reserve(count);

  const std::string commonPrefix(40, 'p');
  for (size_t i = 0; i < count; i++) {
    std::string variant = "abcdefg";
    for (size_t bit = 0; bit < variant.size(); bit++) {
      if ((i >> bit) & 1) variant[bit] = static_cast<char>(variant[bit] - ('a' - 'A'));
    }
    const std::string name = commonPrefix + variant + ".txt";
    records.push_back({makeIndexKey(name.c_str(), SortMode::Name), static_cast<uint32_t>(count - 1 - i), name});
  }
  for (size_t i = 1; i < records.size(); i++) {
    ASSERT_EQ(records[0].key, records[i].key);
    ASSERT_EQ(FsHelpers::naturalCompare(records[0].name.c_str(), records[i].name.c_str()), 0);
  }

  const auto sorted = hybridExternalSort(std::move(records));
  for (size_t i = 1; i < sorted.size(); i++) {
    EXPECT_LT(sorted[i - 1].blobOffset, sorted[i].blobOffset);
  }
}

}  // namespace
