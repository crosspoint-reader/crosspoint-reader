#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "BufferedFileIO.h"
#include "Serialization.h"

// The wrappers batch tiny field reads/writes; every test uses a deliberately small buffer so
// records straddle buffer boundaries and the refill/flush paths are exercised. Wire-format
// compatibility with the plain serialization:: functions is the core contract: streams written
// by either side must read back identically through the other.

namespace {

constexpr size_t kTinyBuffer = 16;  // smaller than most records -> constant refills/flushes

std::string makeString(const size_t len, const char seed) {
  std::string s(len, ' ');
  for (size_t i = 0; i < len; ++i) {
    s[i] = static_cast<char>('a' + ((seed + static_cast<char>(i)) % 26));
  }
  return s;
}

}  // namespace

TEST(BufferedFileIO, WriterProducesSerializationCompatibleStream) {
  FsFile file = HalFile::forReadWrite();

  {
    serialization::BufferedFileWriter writer(file, kTinyBuffer);
    writer.writePod(static_cast<uint32_t>(0xDEADBEEF));
    writer.writeString("short");
    writer.writeString(makeString(100, 3));  // > buffer: exercises the large-write bypass
    writer.writePod(static_cast<int16_t>(-7));
    writer.writeString("");  // zero-length string
    ASSERT_TRUE(writer.flush());
  }

  file.seek(0);
  uint32_t pod32 = 0;
  serialization::readPod(file, pod32);
  EXPECT_EQ(pod32, 0xDEADBEEFu);
  std::string s;
  ASSERT_TRUE(serialization::readString(file, s));
  EXPECT_EQ(s, "short");
  ASSERT_TRUE(serialization::readString(file, s));
  EXPECT_EQ(s, makeString(100, 3));
  int16_t pod16 = 0;
  serialization::readPod(file, pod16);
  EXPECT_EQ(pod16, -7);
  ASSERT_TRUE(serialization::readString(file, s));
  EXPECT_TRUE(s.empty());
}

TEST(BufferedFileIO, ReaderReadsSerializationWrittenStream) {
  FsFile file = HalFile::forReadWrite();
  serialization::writePod(file, static_cast<uint32_t>(42));
  serialization::writeString(file, "hello world");
  serialization::writeString(file, makeString(200, 5));  // > buffer: large-read bypass
  serialization::writePod(file, static_cast<uint8_t>(9));
  file.seek(0);

  serialization::BufferedFileReader reader(file, kTinyBuffer);
  uint32_t pod32 = 0;
  ASSERT_TRUE(reader.readPod(pod32));
  EXPECT_EQ(pod32, 42u);
  std::string s;
  ASSERT_TRUE(reader.readString(s));
  EXPECT_EQ(s, "hello world");
  ASSERT_TRUE(reader.readString(s));
  EXPECT_EQ(s, makeString(200, 5));
  uint8_t pod8 = 0;
  ASSERT_TRUE(reader.readPod(pod8));
  EXPECT_EQ(pod8, 9);
  // Stream exhausted: the next read must fail, not fabricate data.
  EXPECT_FALSE(reader.readPod(pod8));
}

TEST(BufferedFileIO, PositionsMatchUnbufferedFilePositions) {
  FsFile file = HalFile::forReadWrite();

  serialization::BufferedFileWriter writer(file, kTinyBuffer);
  EXPECT_EQ(writer.position(), 0u);
  writer.writePod(static_cast<uint32_t>(1));
  EXPECT_EQ(writer.position(), 4u);
  writer.writeString("abcdef");  // u32 len + 6 bytes
  EXPECT_EQ(writer.position(), 4u + 4u + 6u);
  ASSERT_TRUE(writer.flush());
  EXPECT_EQ(file.position(), 14u);  // flush reconciles the raw file with the logical position

  file.seek(0);
  serialization::BufferedFileReader reader(file, kTinyBuffer);
  EXPECT_EQ(reader.position(), 0u);
  uint32_t pod32 = 0;
  ASSERT_TRUE(reader.readPod(pod32));
  EXPECT_EQ(reader.position(), 4u);
  std::string s;
  ASSERT_TRUE(reader.readString(s));
  EXPECT_EQ(reader.position(), 14u);
}

TEST(BufferedFileIO, SeekServesWindowHitsAndMisses) {
  FsFile file = HalFile::forReadWrite();
  // Record i: u32 marker, at offset i*4.
  for (uint32_t i = 0; i < 64; ++i) {
    serialization::writePod(file, i * 3u);
  }
  file.seek(0);

  serialization::BufferedFileReader reader(file, 32);  // 8 records per window
  uint32_t v = 0;

  // Sequential fill, then seek BACK inside the freshly filled window.
  ASSERT_TRUE(reader.readPod(v));
  ASSERT_TRUE(reader.readPod(v));
  reader.seek(4);  // in-window
  ASSERT_TRUE(reader.readPod(v));
  EXPECT_EQ(v, 3u);

  // Far seek: outside any window.
  reader.seek(60 * 4);
  ASSERT_TRUE(reader.readPod(v));
  EXPECT_EQ(v, 60u * 3u);

  // Back to the start after the far seek.
  reader.seek(0);
  ASSERT_TRUE(reader.readPod(v));
  EXPECT_EQ(v, 0u);
}

TEST(BufferedFileIO, OversizedStringSkipsPayloadAndStaysAligned) {
  FsFile file = HalFile::forReadWrite();
  serialization::writeString(file, "first");
  // Oversized record written by hand: length beyond MAX_STRING_LENGTH plus its payload.
  const uint32_t oversized = serialization::MAX_STRING_LENGTH + 8;
  serialization::writePod(file, oversized);
  const std::string payload(oversized, 'x');
  file.write(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
  serialization::writeString(file, "after");
  file.seek(0);

  serialization::BufferedFileReader reader(file, kTinyBuffer);
  std::string s;
  ASSERT_TRUE(reader.readString(s));
  EXPECT_EQ(s, "first");
  // Oversized: must return false but skip the payload so the stream stays aligned.
  EXPECT_FALSE(reader.readString(s));
  ASSERT_TRUE(reader.readString(s));
  EXPECT_EQ(s, "after");
}

TEST(BufferedFileIO, RoundTripThroughBothWrappers) {
  FsFile file = HalFile::forReadWrite();

  {
    serialization::BufferedFileWriter writer(file, kTinyBuffer);
    for (int i = 0; i < 50; ++i) {
      writer.writeString(makeString(static_cast<size_t>(1 + (i * 7) % 40), static_cast<char>(i)));
      writer.writePod(static_cast<uint32_t>(i * 1000));
    }
    ASSERT_TRUE(writer.flush());
  }

  file.seek(0);
  serialization::BufferedFileReader reader(file, kTinyBuffer);
  for (int i = 0; i < 50; ++i) {
    std::string s;
    ASSERT_TRUE(reader.readString(s)) << "record " << i;
    EXPECT_EQ(s, makeString(static_cast<size_t>(1 + (i * 7) % 40), static_cast<char>(i)));
    uint32_t v = 0;
    ASSERT_TRUE(reader.readPod(v));
    EXPECT_EQ(v, static_cast<uint32_t>(i * 1000));
  }
}

// A reader over a caller's buffer reads exactly what one over its own does, and fills that
// buffer, not one of its own; with no buffer it reads straight through.
TEST(BufferedFileIO, ReaderOverACallersBufferReadsTheSameStream) {
  FsFile file = HalFile::forReadWrite();
  for (uint32_t i = 0; i < 64; ++i) serialization::writeString(file, makeString(i % 23, static_cast<char>(i)));
  file.seek(0);

  uint8_t buffer[kTinyBuffer] = {};
  serialization::BufferedFileReader borrowed(file, buffer, sizeof(buffer));
  std::string s;
  for (uint32_t i = 0; i < 64; ++i) {
    ASSERT_TRUE(borrowed.readString(s)) << "record " << i;
    EXPECT_EQ(s, makeString(i % 23, static_cast<char>(i)));
  }
  bool used = false;
  for (const uint8_t b : buffer) used = used || b != 0;
  EXPECT_TRUE(used) << "the reader must have filled the buffer it was given";

  file.seek(0);
  serialization::BufferedFileReader unbuffered(file, nullptr, sizeof(buffer));
  for (uint32_t i = 0; i < 64; ++i) {
    ASSERT_TRUE(unbuffered.readString(s)) << "record " << i;
    EXPECT_EQ(s, makeString(i % 23, static_cast<char>(i)));
  }
  EXPECT_FALSE(unbuffered.readString(s));
}

// A record read by offset comes from the window when it lies inside it and from the file when it
// does not, and the sequential reading around it carries on unchanged either way.
TEST(BufferedFileIO, ReadStringAtLeavesTheSequentialReadAsItWas) {
  FsFile file = HalFile::forReadWrite();
  std::vector<uint32_t> offsets;
  for (uint32_t i = 0; i < 64; ++i) {
    offsets.push_back(static_cast<uint32_t>(file.position()));
    serialization::writeString(file, makeString(5 + i % 11, static_cast<char>(i)));
  }
  file.seek(0);

  serialization::BufferedFileReader reader(file, 32);
  std::string s;
  for (uint32_t i = 0; i < 64; ++i) {
    // Far back (outside the window) and the one just read (inside it), then on in sequence.
    std::string back;
    ASSERT_TRUE(reader.readStringAt(offsets[i / 2], back));
    EXPECT_EQ(back, makeString(5 + (i / 2) % 11, static_cast<char>(i / 2)));
    if (i > 0) {
      ASSERT_TRUE(reader.readStringAt(offsets[i - 1], back));
      EXPECT_EQ(back, makeString(5 + (i - 1) % 11, static_cast<char>(i - 1)));
    }
    ASSERT_TRUE(reader.readString(s)) << "record " << i;
    EXPECT_EQ(s, makeString(5 + i % 11, static_cast<char>(i))) << "record " << i;
  }
}
