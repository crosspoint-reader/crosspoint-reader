#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ClippingCodec {

inline constexpr char DIRECTORY[] = "/.crosspoint/clippings";
inline constexpr uint16_t VERSION = 3;
inline constexpr size_t HEADER_SIZE = 32;
inline constexpr size_t RECORD_SIZE = 96;
inline constexpr uint16_t MAX_CLIPPINGS_PER_BOOK = 64;
inline constexpr uint16_t MAX_TEXT_BYTES = 512;
inline constexpr uint16_t MAX_CHAPTER_TITLE_BYTES = 64;
inline constexpr uint16_t MAX_BOOK_TITLE_BYTES = 256;
inline constexpr uint16_t MAX_BOOK_AUTHOR_BYTES = 256;
inline constexpr uint16_t MAX_BOOK_PATH_BYTES = 1024;
inline constexpr uint8_t MAX_BOOK_TYPE_BYTES = 8;
inline constexpr uint32_t MAX_FILE_BYTES = 48 * 1024;

enum class Status : uint8_t {
  Ok,
  IoError,
  BadMagic,
  UnsupportedVersion,
  NewerVersion,
  Truncated,
  Corrupt,
  LimitExceeded,
  InvalidUtf8,
};

enum class Format : uint8_t {
  Current,
  CrossInkV1,
  CrossInkV2,
};

struct Source {
  void* context = nullptr;
  uint32_t length = 0;
  bool (*readAt)(void* context, uint32_t offset, uint8_t* data, size_t length) = nullptr;
};

struct BookMetadata {
  std::string title;
  std::string author;
  std::string path;
  std::string bookType = "epub";
};

struct ClippingMetadata {
  uint16_t spineIndex = 0;
  uint16_t startPage = 0;
  uint16_t endPage = 0;
  uint16_t pageCount = 1;
  uint16_t startWordIndex = 0;
  uint16_t endWordIndex = 0;
  uint16_t wordCount = 0;
  uint16_t paragraphIndex = UINT16_MAX;
  uint32_t timestamp = 0;
  uint32_t textOffset = 0;
  uint16_t textLength = 0;
  std::string chapterTitle;
  // CRC of the complete rendered start-page text/geometry. Zero denotes
  // imported legacy data without an exact start-page identity; such clippings
  // remain readable but are never highlighted or opened by guessing.
  uint32_t pageFingerprint = 0;
  // Fixed-layout EPUB positions use page/word fields above. Plain-text books
  // additionally store exact source-byte anchors so changing font, margins or
  // orientation cannot move a clipping to unrelated text. These values live
  // in the record's chapter-title reserve when the title is empty, preserving
  // the existing v3 record size and compatibility with older readers.
  bool hasTextAnchor = false;
  uint32_t textSourceStart = 0;
  uint32_t textSourceEnd = 0;
};

struct Index {
  Format format = Format::Current;
  uint32_t fileLength = 0;
  BookMetadata book;
  std::vector<ClippingMetadata> clippings;
};

uint32_t crc32(const uint8_t* data, size_t length, uint32_t seed = 0);
bool isValidUtf8(std::string_view text);
std::string filePathForBook(std::string_view bookPath, std::string_view bookType = "epub");

Status encodeBookMetadata(const BookMetadata& metadata, std::vector<uint8_t>& out);
Status encodeRecord(const ClippingMetadata& clipping, std::array<uint8_t, RECORD_SIZE>& out);
void encodeHeader(uint16_t count, uint32_t fileLength, uint32_t payloadChecksum, uint32_t metadataLength,
                  uint32_t recordsOffset, uint32_t textsOffset, std::array<uint8_t, HEADER_SIZE>& out);

Status inspect(const Source& source, Index& out);
Status readText(const Source& source, const ClippingMetadata& clipping, std::string& out);

}  // namespace ClippingCodec
