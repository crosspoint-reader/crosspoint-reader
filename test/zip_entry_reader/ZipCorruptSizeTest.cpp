#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "ZipFile.h"

// A one-entry ZIP whose stored entry claims an uncompressed size of 0xFFFFFFFF (corrupt, or the
// ZIP64 marker): readFileToMemory with a trailing NUL used to wrap that size + 1 to a 0-byte
// allocation and read the entry into it. Run under the ASan build to see the overflow it guards.

namespace {

void put16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v));
  out.push_back(static_cast<uint8_t>(v >> 8));
}

void put32(std::vector<uint8_t>& out, uint32_t v) {
  for (int i = 0; i < 4; i++) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

// Writes a ZIP holding one STORED entry `name` with `data`, declaring `uncompressedSize`.
std::string writeZip(const std::string& name, const std::string& data, uint32_t uncompressedSize) {
  std::vector<uint8_t> zip;
  const auto nameLen = static_cast<uint16_t>(name.size());
  const auto dataLen = static_cast<uint32_t>(data.size());

  // Local file header.
  put32(zip, 0x04034b50);
  put16(zip, 20);  // version needed
  put16(zip, 0);   // flags
  put16(zip, 0);   // method: stored
  put16(zip, 0);   // time
  put16(zip, 0);   // date
  put32(zip, 0);   // crc32 (not checked by readFileToMemory)
  put32(zip, dataLen);
  put32(zip, uncompressedSize);
  put16(zip, nameLen);
  put16(zip, 0);  // extra length
  zip.insert(zip.end(), name.begin(), name.end());
  zip.insert(zip.end(), data.begin(), data.end());

  // Central directory.
  const auto cdOffset = static_cast<uint32_t>(zip.size());
  put32(zip, 0x02014b50);
  put16(zip, 20);  // version made by
  put16(zip, 20);  // version needed
  put16(zip, 0);   // flags
  put16(zip, 0);   // method: stored
  put16(zip, 0);   // time
  put16(zip, 0);   // date
  put32(zip, 0);   // crc32
  put32(zip, dataLen);
  put32(zip, uncompressedSize);
  put16(zip, nameLen);
  put16(zip, 0);  // extra length
  put16(zip, 0);  // comment length
  put16(zip, 0);  // disk number
  put16(zip, 0);  // internal attributes
  put32(zip, 0);  // external attributes
  put32(zip, 0);  // local header offset
  zip.insert(zip.end(), name.begin(), name.end());
  const auto cdSize = static_cast<uint32_t>(zip.size()) - cdOffset;

  // End of central directory.
  put32(zip, 0x06054b50);
  put16(zip, 0);  // this disk
  put16(zip, 0);  // central directory disk
  put16(zip, 1);  // entries on this disk
  put16(zip, 1);  // entries total
  put32(zip, cdSize);
  put32(zip, cdOffset);
  put16(zip, 0);  // comment length

  const std::string path = ::testing::TempDir() + "zip_corrupt_size_" + std::to_string(uncompressedSize) + ".zip";
  FILE* f = fopen(path.c_str(), "wb");
  EXPECT_NE(f, nullptr);
  if (f != nullptr) {
    fwrite(zip.data(), 1, zip.size(), f);
    fclose(f);
  }
  return path;
}

}  // namespace

TEST(ZipCorruptSize, ReadsAnHonestStoredEntry) {
  const std::string path = writeZip("a.txt", "hello", 5);
  ZipFile zip{path};
  size_t size = 0;
  uint8_t* data = zip.readFileToMemory("a.txt", &size, true);
  ASSERT_NE(data, nullptr);
  EXPECT_EQ(size, 5u);
  EXPECT_EQ(std::string(reinterpret_cast<char*>(data), 5), "hello");
  EXPECT_EQ(data[5], 0);
  free(data);
}

TEST(ZipCorruptSize, RefusesAnEntryWhoseSizePlusNulWraps) {
  const std::string path = writeZip("a.txt", "hello, this is more than nothing", 0xFFFFFFFFu);
  ZipFile zip{path};
  size_t size = 0;
  EXPECT_EQ(zip.readFileToMemory("a.txt", &size, true), nullptr);
}
