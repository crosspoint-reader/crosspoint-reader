#include <InflateReader.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "BuildArena.h"
#include "ZipFile.h"

// Path to moby-dick.epub injected by CMake via FIXTURE_EPUB define.
#ifndef FIXTURE_EPUB
#error "FIXTURE_EPUB must be defined (path to test/fixtures/moby-dick.epub)"
#endif

// A deflated spine chapter from moby-dick.epub (confirmed present).
static const char* const kTestEntry = "OEBPS/3308270851576161417_2701-h-0.htm.html";

class ZipEntryReaderTest : public ::testing::Test {
 protected:
  std::string epubPath_{FIXTURE_EPUB};
  ZipFile zip_{epubPath_};
};

// Helper: drain an EntryReader into a byte vector.
static bool drain(ZipFile::EntryReader& reader, std::vector<uint8_t>& out) {
  uint8_t buf[512];
  bool done = false;
  while (!done) {
    size_t produced = 0;
    if (!reader.step(buf, sizeof(buf), &produced, &done)) return false;
    out.insert(out.end(), buf, buf + produced);
  }
  return true;
}

// Helper: drain readFileToStream into a byte vector via a Print sink.
class VectorSink : public Print {
 public:
  std::vector<uint8_t> data;
  size_t write(uint8_t b) override {
    data.push_back(b);
    return 1;
  }
  size_t write(const uint8_t* buf, size_t n) override {
    data.insert(data.end(), buf, buf + n);
    return n;
  }
};

// Chunked EntryReader output matches one-shot readFileToStream.
TEST_F(ZipEntryReaderTest, ChunkedOutputMatchesOneShot) {
  // Reference: one-shot readFileToStream
  VectorSink ref;
  ASSERT_TRUE(zip_.readFileToStream(kTestEntry, ref, 1024));
  ASSERT_GT(ref.data.size(), 0u);

  // Under test: EntryReader in small 97-byte chunks (prime, stresses boundary)
  ZipFile::EntryReader reader(zip_);
  ASSERT_TRUE(reader.open(kTestEntry));
  EXPECT_TRUE(reader.isOpen());
  EXPECT_EQ(reader.inflatedSize(), ref.data.size());

  std::vector<uint8_t> got;
  ASSERT_TRUE(drain(reader, got));

  EXPECT_EQ(got.size(), ref.data.size());
  EXPECT_EQ(got, ref.data);
  EXPECT_EQ(reader.bytesProduced(), ref.data.size());
}

// Single large step (cap > inflatedSize) still terminates correctly.
TEST_F(ZipEntryReaderTest, SingleLargeStep) {
  ZipFile::EntryReader reader(zip_);
  ASSERT_TRUE(reader.open(kTestEntry));

  std::vector<uint8_t> buf(reader.inflatedSize() + 64);
  size_t produced = 0;
  bool done = false;
  ASSERT_TRUE(reader.step(buf.data(), buf.size(), &produced, &done));
  // May finish in one step (Done) or need another (Ok with produced < cap).
  // Either way, draining must complete without error.
  std::vector<uint8_t> all(buf.data(), buf.data() + produced);
  while (!done) {
    ASSERT_TRUE(reader.step(buf.data(), buf.size(), &produced, &done));
    all.insert(all.end(), buf.data(), buf.data() + produced);
  }

  VectorSink ref;
  ASSERT_TRUE(zip_.readFileToStream(kTestEntry, ref, 1024));
  EXPECT_EQ(all, ref.data);
}

// Re-opening the same reader on a different entry works correctly.
TEST_F(ZipEntryReaderTest, ReOpen) {
  // Open the CSS file (a short stored or deflated entry).
  const char* kCssEntry = "OEBPS/pgepub.css";

  ZipFile::EntryReader reader(zip_);
  ASSERT_TRUE(reader.open(kTestEntry));
  std::vector<uint8_t> first;
  ASSERT_TRUE(drain(reader, first));

  ASSERT_TRUE(reader.open(kCssEntry));
  std::vector<uint8_t> css;
  ASSERT_TRUE(drain(reader, css));

  VectorSink cssRef;
  ASSERT_TRUE(zip_.readFileToStream(kCssEntry, cssRef, 1024));
  EXPECT_EQ(css, cssRef.data);

  // Re-open the first entry and confirm it still matches.
  ASSERT_TRUE(reader.open(kTestEntry));
  std::vector<uint8_t> again;
  ASSERT_TRUE(drain(reader, again));
  EXPECT_EQ(again, first);
}

// close() leaves the reader in a safe state; isOpen() reflects it.
TEST_F(ZipEntryReaderTest, CloseResetsState) {
  ZipFile::EntryReader reader(zip_);
  ASSERT_TRUE(reader.open(kTestEntry));
  EXPECT_TRUE(reader.isOpen());
  reader.close();
  EXPECT_FALSE(reader.isOpen());
  EXPECT_EQ(reader.bytesProduced(), 0u);
  EXPECT_EQ(reader.inflatedSize(), 0u);
}

// Arena-backed reader (readBuf + inflate ring carved from a BuildArena) must
// produce byte-identical output, reclaim its whole scope on close, and never
// touch malloc for the ring (plan Phase 2, EntryReader site).
// open(stat, cap): the first `cap` bytes exactly, then done -- from a ring sized to the cap, not
// to the entry. This is what lets a JPEG header walk over a 400 KB entry cost a 16 KB ring: a
// deflate back-reference never reaches further back than the bytes produced so far.
TEST_F(ZipEntryReaderTest, OutputCapStopsAtTheCapFromACapSizedRing) {
  VectorSink ref;
  ASSERT_TRUE(zip_.readFileToStream(kTestEntry, ref, 1024));
  constexpr size_t kCap = 3000;
  ASSERT_GT(ref.data.size(), 2 * kCap) << "the entry must be much larger than the cap";

  ZipFile::FileStatSlim stat = {};
  ASSERT_TRUE(zip_.loadFileStatSlim(kTestEntry, &stat));

  BuildArena arena(40 * 1024);
  ASSERT_TRUE(arena.valid());
  {
    ZipFile::EntryReader reader(zip_, 512, &arena);
    ASSERT_TRUE(reader.open(stat, kCap));
    // 512 B chunk + a ring for 3000 B of output (+ alignment): nowhere near the 32 KB ring.
    EXPECT_LE(arena.highWater(), 512u + InflateReader::ringSizeFor(kCap) + 64u);

    std::vector<uint8_t> got;
    ASSERT_TRUE(drain(reader, got));
    ASSERT_EQ(got.size(), kCap);
    EXPECT_TRUE(std::equal(got.begin(), got.end(), ref.data.begin())) << "the capped read is an exact prefix";
    EXPECT_EQ(reader.bytesProduced(), kCap);
    EXPECT_EQ(reader.inflatedSize(), ref.data.size()) << "the entry's real size is still reported";
  }
  EXPECT_EQ(arena.used(), 0u);

  // A cap past the end changes nothing.
  ZipFile::EntryReader whole(zip_, 512);
  ASSERT_TRUE(whole.open(stat, ref.data.size() * 2));
  std::vector<uint8_t> all;
  ASSERT_TRUE(drain(whole, all));
  EXPECT_EQ(all, ref.data);
}

TEST_F(ZipEntryReaderTest, ArenaBackedOutputMatchesAndReclaims) {
  VectorSink ref;
  ASSERT_TRUE(zip_.readFileToStream(kTestEntry, ref, 1024));
  ASSERT_GT(ref.data.size(), 0u);

  BuildArena arena(40 * 1024);  // 1 KB readBuf + <=32 KB ring + alignment
  ASSERT_TRUE(arena.valid());
  const size_t before = arena.used();
  {
    ZipFile::EntryReader reader(zip_, 1024, &arena);
    ASSERT_TRUE(reader.open(kTestEntry));
    EXPECT_GT(arena.used(), before) << "ring/readBuf should come from the arena";

    std::vector<uint8_t> got;
    ASSERT_TRUE(drain(reader, got));
    EXPECT_EQ(got, ref.data);
  }
  EXPECT_EQ(arena.used(), before) << "reader close must release its arena scope";
  EXPECT_EQ(arena.failedAllocSize(), 0u);
  EXPECT_LE(arena.highWater(), 34u * 1024u);
}

// Arena too small for the ring: open must fail cleanly and release the scope.
TEST_F(ZipEntryReaderTest, ArenaTooSmallFailsCleanly) {
  BuildArena arena(2 * 1024);  // fits readBuf, not the ring
  ASSERT_TRUE(arena.valid());
  ZipFile::EntryReader reader(zip_, 1024, &arena);
  EXPECT_FALSE(reader.open(kTestEntry));
  EXPECT_EQ(arena.used(), 0u);
  EXPECT_GT(arena.failedAllocSize(), 0u);
}

TEST_F(ZipEntryReaderTest, CloseCannotReleaseNewerCallerBlock) {
  BuildArena arena(40 * 1024);
  ZipFile::EntryReader reader(zip_, 1024, &arena);
  ASSERT_TRUE(reader.open(kTestEntry));
  const size_t readerUsed = arena.used();

  auto callerBlock = arena.reserveBlock();
  ASSERT_NE(arena.alloc(64, 1), nullptr);
  reader.close();
  EXPECT_GT(arena.used(), readerUsed);

  EXPECT_TRUE(arena.release(callerBlock));
  reader.close();
  EXPECT_EQ(arena.used(), 0u);
}

// Records how long a write the zip asked for, without touching the bytes: a SIZE_MAX length must
// show up as a number in the test, not as a 4 GB memcpy that takes the test binary down.
class LengthRecordingSink : public Print {
 public:
  std::vector<uint8_t> data;
  size_t largestWrite = 0;
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t* buf, size_t n) override {
    if (n > largestWrite) largestWrite = n;
    if (n > 1024 * 1024) return 0;  // nothing this test streams is that long
    data.insert(data.end(), buf, buf + n);
    return n;
  }
};

// Where a STORED entry's bytes start: its local header (30 bytes, name and extra lengths at 26/28)
// sits at offset 0 for the first entry of an EPUB, which is always the stored "mimetype".
static long firstEntryDataOffset(const std::string& path) {
  FILE* fp = std::fopen(path.c_str(), "rb");
  if (!fp) return -1;
  uint8_t header[30];
  const size_t got = std::fread(header, 1, sizeof(header), fp);
  std::fclose(fp);
  if (got != sizeof(header)) return -1;
  const long nameLen = header[26] | (header[27] << 8);
  const long extraLen = header[28] | (header[29] << 8);
  return 30 + nameLen + extraLen;
}

// HalFile::read() reports an error as -1. readFileToStream's STORED path kept it in a size_t, so
// the failed read passed its `== 0` check and reached the sink as a SIZE_MAX-byte write.
TEST_F(ZipEntryReaderTest, FailedStoredReadIsNotStreamedAsALength) {
  const long dataOffset = firstEntryDataOffset(epubPath_);
  ASSERT_GT(dataOffset, 30);

  LengthRecordingSink intact;
  ASSERT_TRUE(zip_.readFileToStream("mimetype", intact, 1024));
  EXPECT_EQ(std::string(intact.data.begin(), intact.data.end()), "application/epub+zip");

  HalFile::failReadAt = dataOffset;
  LengthRecordingSink sink;
  const bool ok = zip_.readFileToStream("mimetype", sink, 1024);
  HalFile::failReadAt = -1;

  EXPECT_FALSE(ok) << "a failed read must fail the stream";
  EXPECT_LE(sink.largestWrite, 1024u) << "the -1 from read() reached the sink as a length";
  EXPECT_TRUE(sink.data.empty());
}
