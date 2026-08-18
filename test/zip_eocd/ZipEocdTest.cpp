#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "ZipEocd.h"

// Builds a synthetic file tail: [padding][EOCD record][comment]. The real
// EOCD's fields are filled in consistently; the comment bytes are caller-
// supplied so tests can embed decoy signatures inside it.
namespace {

std::vector<uint8_t> makeEocdRecord(uint16_t totalEntries, uint32_t centralDirOffset, uint16_t commentLength) {
  std::vector<uint8_t> record(ZIP_EOCD_MIN_SIZE, 0);
  const uint32_t signature = ZIP_EOCD_SIGNATURE;
  memcpy(record.data(), &signature, sizeof(signature));
  memcpy(record.data() + 8, &totalEntries, sizeof(totalEntries));   // entries on this disk
  memcpy(record.data() + 10, &totalEntries, sizeof(totalEntries));  // total entries
  memcpy(record.data() + 16, &centralDirOffset, sizeof(centralDirOffset));
  memcpy(record.data() + 20, &commentLength, sizeof(commentLength));
  return record;
}

}  // namespace

TEST(ZipEocdParse, ReadsFieldsLittleEndian) {
  const auto record = makeEocdRecord(7, 0x00012345, 42);
  const ZipEocdCandidate c = parseZipEocdCandidate(record.data());
  EXPECT_EQ(c.totalEntries, 7);
  EXPECT_EQ(c.centralDirOffset, 0x00012345u);
  EXPECT_EQ(c.commentLength, 42);
}

TEST(ZipEocdConsistency, AcceptsRecordWithNoComment) {
  // File: [1000 bytes of archive][EOCD], comment length 0.
  const size_t recordOffset = 1000;
  const size_t fileSize = recordOffset + ZIP_EOCD_MIN_SIZE;
  const auto record = makeEocdRecord(3, 500, 0);
  EXPECT_TRUE(isZipEocdSelfConsistent(parseZipEocdCandidate(record.data()), recordOffset, fileSize));
}

TEST(ZipEocdConsistency, AcceptsRecordWhoseCommentRunsToEof) {
  const size_t recordOffset = 1000;
  const uint16_t commentLength = 200;
  const size_t fileSize = recordOffset + ZIP_EOCD_MIN_SIZE + commentLength;
  const auto record = makeEocdRecord(3, 500, commentLength);
  EXPECT_TRUE(isZipEocdSelfConsistent(parseZipEocdCandidate(record.data()), recordOffset, fileSize));
}

// The regression case from PR #2614 review: a decoy PK\x05\x06 embedded in
// the archive comment sits nearer EOF than the real record. Its "fields" are
// whatever comment bytes happen to follow, so its comment length cannot run
// to EOF from its position (any mismatch fails it), and garbage central
// directory offsets fail the bound check.
TEST(ZipEocdConsistency, RejectsDecoySignatureInsideComment) {
  const size_t realRecordOffset = 1000;
  const uint16_t commentLength = 300;
  const size_t fileSize = realRecordOffset + ZIP_EOCD_MIN_SIZE + commentLength;

  // Decoy 100 bytes into the comment, followed by garbage field bytes.
  const size_t decoyOffset = realRecordOffset + ZIP_EOCD_MIN_SIZE + 100;
  std::vector<uint8_t> decoy(ZIP_EOCD_MIN_SIZE, 0xA5);
  const uint32_t signature = ZIP_EOCD_SIGNATURE;
  memcpy(decoy.data(), &signature, sizeof(signature));

  const ZipEocdCandidate c = parseZipEocdCandidate(decoy.data());
  // 0xA5A5 comment length can't make decoyOffset + 22 + len == fileSize.
  EXPECT_FALSE(isZipEocdSelfConsistent(c, decoyOffset, fileSize));

  // The real record still validates.
  const auto real = makeEocdRecord(3, 500, commentLength);
  EXPECT_TRUE(isZipEocdSelfConsistent(parseZipEocdCandidate(real.data()), realRecordOffset, fileSize));
}

// A byte-exact copy of the real EOCD placed inside the comment: its comment-
// length field equals the real one's, but measured from the copy's position
// the comment can no longer run exactly to EOF, so it must fail.
TEST(ZipEocdConsistency, RejectsExactEocdCopyInsideComment) {
  const size_t realRecordOffset = 1000;
  const uint16_t commentLength = ZIP_EOCD_MIN_SIZE;  // comment holds exactly the copy
  const size_t fileSize = realRecordOffset + ZIP_EOCD_MIN_SIZE + commentLength;
  const auto real = makeEocdRecord(3, 500, commentLength);

  const size_t copyOffset = realRecordOffset + ZIP_EOCD_MIN_SIZE;
  EXPECT_TRUE(isZipEocdSelfConsistent(parseZipEocdCandidate(real.data()), realRecordOffset, fileSize));
  EXPECT_FALSE(isZipEocdSelfConsistent(parseZipEocdCandidate(real.data()), copyOffset, fileSize));
}

TEST(ZipEocdConsistency, RejectsCentralDirBeyondRecord) {
  const size_t recordOffset = 1000;
  const size_t fileSize = recordOffset + ZIP_EOCD_MIN_SIZE;
  // Central directory claimed to start after the record itself: nonsense.
  const auto record = makeEocdRecord(3, static_cast<uint32_t>(recordOffset) + 1, 0);
  EXPECT_FALSE(isZipEocdSelfConsistent(parseZipEocdCandidate(record.data()), recordOffset, fileSize));
}

TEST(ZipEocdConsistency, RejectsTruncatedFile) {
  // Comment length says 100 bytes but the file ends after 40 of them.
  const size_t recordOffset = 1000;
  const auto record = makeEocdRecord(3, 500, 100);
  const size_t fileSize = recordOffset + ZIP_EOCD_MIN_SIZE + 40;
  EXPECT_FALSE(isZipEocdSelfConsistent(parseZipEocdCandidate(record.data()), recordOffset, fileSize));
}
