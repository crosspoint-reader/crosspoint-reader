#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Buffer-level parsing and validation of ZIP end-of-central-directory (EOCD)
// candidates, split out of ZipFile so host tests can exercise them on raw
// bytes (ZipFile itself needs HalStorage). Reads are little-endian, matching
// the ESP32 targets and the CI test hosts.

constexpr size_t ZIP_EOCD_MIN_SIZE = 22;
constexpr uint32_t ZIP_EOCD_SIGNATURE = 0x06054b50;
constexpr uint32_t ZIP_CENTRAL_DIR_SIGNATURE = 0x02014b50;

struct ZipEocdCandidate {
  uint16_t totalEntries;
  uint32_t centralDirOffset;
  uint16_t commentLength;
};

// record must point at ZIP_EOCD_MIN_SIZE readable bytes starting at the
// PK\x05\x06 signature. Field offsets within the record: 10 = total entry
// count (u16), 16 = central directory start offset (u32), 20 = archive
// comment length (u16).
inline ZipEocdCandidate parseZipEocdCandidate(const uint8_t* record) {
  ZipEocdCandidate c;
  memcpy(&c.totalEntries, record + 10, sizeof(c.totalEntries));
  memcpy(&c.centralDirOffset, record + 16, sizeof(c.centralDirOffset));
  memcpy(&c.commentLength, record + 20, sizeof(c.commentLength));
  return c;
}

// The real EOCD's archive comment is the last thing in the file, so its
// comment length runs exactly to end-of-file, and its central directory
// starts at or before the record itself. A PK\x05\x06 that is actually
// payload inside the archive comment sits nearer EOF than the real record
// and reads its "fields" out of comment bytes, so it fails these checks
// unless the comment embeds a forged, correctly positioned record. The
// central-directory probe below rejects forged zero-entry records. A forged
// record pointing into the middle of a real central directory is not
// detected (only a full directory walk would catch it); it only makes that
// crafted file fail to open.
inline bool isZipEocdSelfConsistent(const ZipEocdCandidate& c, size_t recordOffset, size_t fileSize) {
  return recordOffset + ZIP_EOCD_MIN_SIZE + c.commentLength == fileSize && c.centralDirOffset <= recordOffset;
}

// Decides the central-directory probe from the 4 bytes read at the
// candidate's central directory offset. Zero-entry candidates never pass:
// ZipFile only serves EPUBs, which are never empty archives, and a forged
// zero-entry record in the comment can point at the real EOCD, whose
// signature is what an empty archive would have there. Callers may skip the
// read when totalEntries is 0.
inline bool zipEocdProbeAccepts(const ZipEocdCandidate& c, uint32_t signatureAtCentralDirOffset) {
  return c.totalEntries != 0 && signatureAtCentralDirOffset == ZIP_CENTRAL_DIR_SIGNATURE;
}
