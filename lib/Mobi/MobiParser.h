#pragma once

// MOBI (Mobipocket / Kindle MOBI6) parser: PDB container walk, MOBI/EXTH
// headers, and text-record decompression (none / PalmDOC LZ77 / HUFF-CDIC).
//
// Scope: DRM-free MOBI6 (.mobi, .azw, and the MOBI6 half of combo .azw3
// files). Pure-KF8 books (azw3 with no MOBI6 part) and encrypted books are
// rejected with a reason the UI can show. The output is the raw "rawml" HTML
// stream plus image-record access; MobiToEpub turns that into an EPUB the
// existing reader opens natively.
//
// Format references: MobileRead wiki (MOBI, PalmDOC, HUFF/CDIC records).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class HalFile;

class MobiParser {
 public:
  enum class Error : uint8_t {
    None = 0,
    OpenFailed,
    NotPdb,        // not a BOOKMOBI PDB container
    Encrypted,     // DRM — cannot convert
    Kf8Only,       // pure KF8/azw3, no MOBI6 text
    BadHeader,
    Decompress,
    Oom,
  };

  explicit MobiParser(std::string path);
  ~MobiParser();

  // Parses headers only (fast). Returns false and sets error() on failure.
  bool load();

  Error error() const { return err; }
  const char* errorString() const;

  const std::string& title() const { return bookTitle; }
  const std::string& author() const { return bookAuthor; }

  // Decompressed size of the full HTML stream (from the PalmDOC header).
  uint32_t rawTextLength() const { return textLength; }

  // True when the MOBI header declares UTF-8 text (65001); false = cp1252.
  bool isUtf8() const { return textEncoding == 65001; }

  // Decompresses the whole text stream into caller-supplied buffer (must be
  // at least rawTextLength() bytes; lands in PSRAM via big-alloc malloc).
  // Returns actual byte count (<= rawTextLength) or 0 on failure.
  uint32_t readRawText(uint8_t* dst, uint32_t dstLen);

  // Image records: index is the 1-based "recindex" attribute value used in
  // MOBI HTML. Returns record bytes (JPEG/PNG/GIF/BMP as stored) or empty.
  uint16_t imageCount() const;
  std::vector<uint8_t> readImage(uint16_t recindex);
  // EXTH 201 cover offset (relative recindex), -1 if none.
  int32_t coverRecindex() const { return coverOffset; }

 private:
  bool readRecordOffsets();
  bool readRecord(uint16_t index, std::vector<uint8_t>& out);
  bool parseRecord0();
  bool parseHuffCdic();
  uint32_t trimTrailingEntries(const uint8_t* rec, uint32_t len) const;

  // Decompressors: return bytes written to dst, 0 on error.
  uint32_t decompressPalmDoc(const uint8_t* src, uint32_t srcLen, uint8_t* dst, uint32_t dstCap) const;
  uint32_t decompressHuffCdic(const uint8_t* src, uint32_t srcLen, uint8_t* dst, uint32_t dstCap) const;

  std::string path;
  std::unique_ptr<HalFile> file;
  Error err = Error::None;

  // PDB
  uint16_t numRecords = 0;
  std::vector<uint32_t> recordOffsets;  // numRecords + 1 (file size sentinel)

  // PalmDOC / MOBI header
  uint16_t compression = 0;  // 1 none, 2 PalmDOC, 17480 HUFF/CDIC
  uint32_t textLength = 0;
  uint16_t textRecordCount = 0;
  uint16_t recordSize = 4096;
  uint32_t textEncoding = 65001;
  uint16_t firstImageRecord = 0;  // absolute PDB record index
  uint16_t lastContentRecord = 0;
  uint32_t extraDataFlags = 0;
  uint32_t huffRecord = 0;
  uint32_t huffCount = 0;
  int32_t coverOffset = -1;

  std::string bookTitle;
  std::string bookAuthor;

  // HUFF/CDIC tables (built lazily when compression == 17480)
  struct HuffCdic {
    uint32_t table1[256];
    uint32_t mincode[33];
    uint32_t maxcode[33];
    std::vector<std::vector<uint8_t>> dicts;  // raw CDIC records
    uint32_t codeLength = 0;
    bool ready = false;
  };
  std::unique_ptr<HuffCdic> huff;
};
