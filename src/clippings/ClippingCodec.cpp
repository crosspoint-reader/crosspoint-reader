#include "ClippingCodec.h"

#include <algorithm>
#include <array>
#include <limits>

namespace ClippingCodec {
namespace {

constexpr std::array<uint8_t, 4> MAGIC = {'C', 'V', 'C', 'L'};
constexpr size_t LEGACY_CHAPTER_TITLE_BYTES = 48;
constexpr std::array<uint32_t, 16> CRC32_NIBBLE = {
    0x00000000U, 0x1DB71064U, 0x3B6E20C8U, 0x26D930ACU, 0x76DC4190U, 0x6B6B51F4U, 0x4DB26158U, 0x5005713CU,
    0xEDB88320U, 0xF00F9344U, 0xD6D6A3E8U, 0xCB61B38CU, 0x9B64C2B0U, 0x86D3D2D4U, 0xA00AE278U, 0xBDBDF21CU,
};

uint16_t readU16(const uint8_t* data) { return static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8); }

uint32_t readU32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

void writeU16(uint8_t* data, const uint16_t value) {
  data[0] = static_cast<uint8_t>(value);
  data[1] = static_cast<uint8_t>(value >> 8);
}

void writeU32(uint8_t* data, const uint32_t value) {
  data[0] = static_cast<uint8_t>(value);
  data[1] = static_cast<uint8_t>(value >> 8);
  data[2] = static_cast<uint8_t>(value >> 16);
  data[3] = static_cast<uint8_t>(value >> 24);
}

bool readExact(const Source& source, const uint32_t offset, uint8_t* data, const size_t length) {
  if (!source.readAt || offset > source.length || length > source.length - offset) return false;
  return source.readAt(source.context, offset, data, length);
}

bool addWouldOverflow(const uint32_t left, const uint32_t right) {
  return right > std::numeric_limits<uint32_t>::max() - left;
}

bool validBookType(const std::string_view value) {
  if (value.empty() || value.size() > MAX_BOOK_TYPE_BYTES) return false;
  return std::all_of(value.begin(), value.end(),
                     [](const unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); });
}

Status validateString(const std::string_view value, const size_t maxBytes) {
  if (value.size() > maxBytes) return Status::LimitExceeded;
  return isValidUtf8(value) ? Status::Ok : Status::InvalidUtf8;
}

Status validateClipping(const ClippingMetadata& clipping) {
  if (clipping.textLength > MAX_TEXT_BYTES || clipping.chapterTitle.size() > MAX_CHAPTER_TITLE_BYTES) {
    return Status::LimitExceeded;
  }
  if (clipping.pageCount == 0 || clipping.startPage > clipping.endPage || clipping.endPage >= clipping.pageCount ||
      clipping.wordCount == 0 || clipping.textLength == 0) {
    return Status::Corrupt;
  }
  if (clipping.startPage == clipping.endPage && clipping.startWordIndex > clipping.endWordIndex) {
    return Status::Corrupt;
  }
  return isValidUtf8(clipping.chapterTitle) ? Status::Ok : Status::InvalidUtf8;
}

Status readSizedString16(const Source& source, uint32_t& cursor, const uint32_t end, const size_t maxLength,
                         std::string& out) {
  std::array<uint8_t, 2> lengthBytes{};
  if (cursor > end || end - cursor < lengthBytes.size() ||
      !readExact(source, cursor, lengthBytes.data(), lengthBytes.size())) {
    return Status::Truncated;
  }
  cursor += lengthBytes.size();
  const uint16_t length = readU16(lengthBytes.data());
  if (length > maxLength) return Status::LimitExceeded;
  if (cursor > end || length > end - cursor) return Status::Truncated;
  out.resize(length);
  if (length > 0 && !readExact(source, cursor, reinterpret_cast<uint8_t*>(out.data()), length)) {
    out.clear();
    return Status::IoError;
  }
  cursor += length;
  return isValidUtf8(out) ? Status::Ok : Status::InvalidUtf8;
}

Status readSizedString32(const Source& source, uint32_t& cursor, const size_t maxLength, std::string& out) {
  std::array<uint8_t, 4> lengthBytes{};
  if (!readExact(source, cursor, lengthBytes.data(), lengthBytes.size())) return Status::Truncated;
  cursor += lengthBytes.size();
  const uint32_t length = readU32(lengthBytes.data());
  if (length > maxLength) return Status::LimitExceeded;
  if (cursor > source.length || length > source.length - cursor) return Status::Truncated;
  out.resize(length);
  if (length > 0 && !readExact(source, cursor, reinterpret_cast<uint8_t*>(out.data()), length)) {
    out.clear();
    return Status::IoError;
  }
  cursor += length;
  return isValidUtf8(out) ? Status::Ok : Status::InvalidUtf8;
}

Status validateStoredText(const Source& source, const ClippingMetadata& clipping) {
  std::array<uint8_t, MAX_TEXT_BYTES> buffer{};
  if (!readExact(source, clipping.textOffset, buffer.data(), clipping.textLength)) return Status::Truncated;
  return isValidUtf8(std::string_view(reinterpret_cast<const char*>(buffer.data()), clipping.textLength))
             ? Status::Ok
             : Status::InvalidUtf8;
}

Status inspectCurrent(const Source& source, Index& out) {
  if (source.length < HEADER_SIZE) return Status::Truncated;
  std::array<uint8_t, HEADER_SIZE> header{};
  if (!readExact(source, 0, header.data(), header.size())) return Status::IoError;
  if (!std::equal(MAGIC.begin(), MAGIC.end(), header.begin())) return Status::BadMagic;

  const uint16_t version = readU16(header.data() + 4);
  if (version > VERSION) return Status::NewerVersion;
  if (version != VERSION) return Status::UnsupportedVersion;
  if (readU16(header.data() + 6) != HEADER_SIZE) return Status::Corrupt;

  const uint32_t fileLength = readU32(header.data() + 8);
  const uint32_t expectedChecksum = readU32(header.data() + 12);
  const uint16_t count = readU16(header.data() + 16);
  const uint16_t reserved = readU16(header.data() + 18);
  const uint32_t metadataLength = readU32(header.data() + 20);
  const uint32_t recordsOffset = readU32(header.data() + 24);
  const uint32_t textsOffset = readU32(header.data() + 28);

  if (reserved != 0 || fileLength != source.length || fileLength > MAX_FILE_BYTES || count > MAX_CLIPPINGS_PER_BOOK ||
      recordsOffset != HEADER_SIZE + metadataLength || recordsOffset > fileLength ||
      static_cast<uint64_t>(recordsOffset) + static_cast<uint64_t>(count) * RECORD_SIZE != textsOffset ||
      textsOffset > fileLength) {
    return count > MAX_CLIPPINGS_PER_BOOK ? Status::LimitExceeded : Status::Corrupt;
  }

  std::array<uint8_t, 128> checksumBuffer{};
  uint32_t actualChecksum = 0;
  uint32_t checksumOffset = HEADER_SIZE;
  while (checksumOffset < fileLength) {
    const size_t chunk = std::min<size_t>(checksumBuffer.size(), fileLength - checksumOffset);
    if (!readExact(source, checksumOffset, checksumBuffer.data(), chunk)) return Status::IoError;
    actualChecksum = crc32(checksumBuffer.data(), chunk, actualChecksum);
    checksumOffset += static_cast<uint32_t>(chunk);
  }
  if (actualChecksum != expectedChecksum) return Status::Corrupt;

  Index parsed;
  parsed.format = Format::Current;
  parsed.fileLength = fileLength;
  uint32_t cursor = HEADER_SIZE;
  Status status = readSizedString16(source, cursor, recordsOffset, MAX_BOOK_TITLE_BYTES, parsed.book.title);
  if (status != Status::Ok) return status;
  status = readSizedString16(source, cursor, recordsOffset, MAX_BOOK_AUTHOR_BYTES, parsed.book.author);
  if (status != Status::Ok) return status;
  status = readSizedString16(source, cursor, recordsOffset, MAX_BOOK_PATH_BYTES, parsed.book.path);
  if (status != Status::Ok) return status;

  uint8_t bookTypeLength = 0;
  if (cursor >= recordsOffset || !readExact(source, cursor, &bookTypeLength, 1)) return Status::Truncated;
  cursor++;
  if (bookTypeLength == 0 || bookTypeLength > MAX_BOOK_TYPE_BYTES || bookTypeLength > recordsOffset - cursor) {
    return Status::Corrupt;
  }
  parsed.book.bookType.resize(bookTypeLength);
  if (!readExact(source, cursor, reinterpret_cast<uint8_t*>(parsed.book.bookType.data()), bookTypeLength)) {
    return Status::IoError;
  }
  cursor += bookTypeLength;
  if (cursor != recordsOffset || !validBookType(parsed.book.bookType)) return Status::Corrupt;

  parsed.clippings.reserve(count);
  uint32_t expectedTextOffset = textsOffset;
  std::array<uint8_t, RECORD_SIZE> record{};
  for (uint16_t i = 0; i < count; ++i) {
    const uint32_t offset = recordsOffset + static_cast<uint32_t>(i) * RECORD_SIZE;
    if (!readExact(source, offset, record.data(), record.size())) return Status::Truncated;

    ClippingMetadata clipping;
    clipping.spineIndex = readU16(record.data());
    clipping.startPage = readU16(record.data() + 2);
    clipping.endPage = readU16(record.data() + 4);
    clipping.pageCount = readU16(record.data() + 6);
    clipping.startWordIndex = readU16(record.data() + 8);
    clipping.endWordIndex = readU16(record.data() + 10);
    clipping.wordCount = readU16(record.data() + 12);
    clipping.paragraphIndex = readU16(record.data() + 14);
    clipping.timestamp = readU32(record.data() + 16);
    clipping.textOffset = readU32(record.data() + 20);
    clipping.textLength = readU16(record.data() + 24);
    const uint16_t chapterLength = readU16(record.data() + 26);
    if (chapterLength > MAX_CHAPTER_TITLE_BYTES) return Status::Corrupt;
    clipping.chapterTitle.assign(reinterpret_cast<const char*>(record.data() + 28), chapterLength);
    clipping.pageFingerprint = readU32(record.data() + 92);

    if (clipping.textOffset != expectedTextOffset || addWouldOverflow(clipping.textOffset, clipping.textLength) ||
        clipping.textOffset + clipping.textLength > fileLength) {
      return Status::Corrupt;
    }
    status = validateClipping(clipping);
    if (status != Status::Ok) return status;
    status = validateStoredText(source, clipping);
    if (status != Status::Ok) return status;
    expectedTextOffset += clipping.textLength;
    parsed.clippings.push_back(std::move(clipping));
  }
  if (expectedTextOffset != fileLength) return Status::Corrupt;

  out = std::move(parsed);
  return Status::Ok;
}

Status inspectLegacy(const Source& source, const uint8_t version, Index& out) {
  if (source.length > MAX_FILE_BYTES) return Status::LimitExceeded;
  if (source.length < 3) return Status::Truncated;
  std::array<uint8_t, 3> prefix{};
  if (!readExact(source, 0, prefix.data(), prefix.size())) return Status::IoError;
  const uint16_t count = readU16(prefix.data() + 1);
  if (count > MAX_CLIPPINGS_PER_BOOK) return Status::LimitExceeded;

  Index parsed;
  parsed.format = version == 1 ? Format::CrossInkV1 : Format::CrossInkV2;
  parsed.fileLength = source.length;
  parsed.book.bookType = "epub";
  uint32_t cursor = 3;
  Status status = readSizedString32(source, cursor, MAX_BOOK_TITLE_BYTES, parsed.book.title);
  if (status != Status::Ok) return status;
  status = readSizedString32(source, cursor, MAX_BOOK_AUTHOR_BYTES, parsed.book.author);
  if (status != Status::Ok) return status;
  status = readSizedString32(source, cursor, MAX_BOOK_PATH_BYTES, parsed.book.path);
  if (status != Status::Ok) return status;

  parsed.clippings.reserve(count);
  constexpr size_t fixedLegacyRecordBytes = 8 * sizeof(uint16_t) + sizeof(uint32_t) + LEGACY_CHAPTER_TITLE_BYTES;
  std::array<uint8_t, fixedLegacyRecordBytes> record{};
  for (uint16_t i = 0; i < count; ++i) {
    if (!readExact(source, cursor, record.data(), record.size())) return Status::Truncated;
    cursor += record.size();
    ClippingMetadata clipping;
    clipping.spineIndex = readU16(record.data());
    clipping.startPage = readU16(record.data() + 2);
    clipping.endPage = readU16(record.data() + 4);
    clipping.pageCount = readU16(record.data() + 6);
    clipping.startWordIndex = readU16(record.data() + 8);
    clipping.endWordIndex = readU16(record.data() + 10);
    clipping.wordCount = readU16(record.data() + 12);
    clipping.paragraphIndex = readU16(record.data() + 14);
    clipping.timestamp = readU32(record.data() + 16);
    const char* chapter = reinterpret_cast<const char*>(record.data() + 20);
    const auto nul = std::find(chapter, chapter + LEGACY_CHAPTER_TITLE_BYTES, '\0');
    if (nul == chapter + LEGACY_CHAPTER_TITLE_BYTES) return Status::Corrupt;
    clipping.chapterTitle.assign(chapter, nul);

    std::array<uint8_t, 4> textLengthBytes{};
    const size_t lengthBytes = version == 1 ? 4 : 2;
    if (!readExact(source, cursor, textLengthBytes.data(), lengthBytes)) return Status::Truncated;
    cursor += lengthBytes;
    const uint32_t textLength = version == 1 ? readU32(textLengthBytes.data()) : readU16(textLengthBytes.data());
    if (textLength == 0 || textLength > MAX_TEXT_BYTES) return Status::LimitExceeded;
    clipping.textOffset = cursor;
    clipping.textLength = static_cast<uint16_t>(textLength);
    if (cursor > source.length || textLength > source.length - cursor) return Status::Truncated;

    status = validateClipping(clipping);
    if (status != Status::Ok) return status;
    status = validateStoredText(source, clipping);
    if (status != Status::Ok) return status;
    cursor += textLength;
    parsed.clippings.push_back(std::move(clipping));
  }
  if (cursor != source.length) return Status::Corrupt;

  out = std::move(parsed);
  return Status::Ok;
}

}  // namespace

uint32_t crc32(const uint8_t* data, const size_t length, const uint32_t seed) {
  uint32_t value = ~seed;
  for (size_t i = 0; i < length; ++i) {
    value ^= data[i];
    value = (value >> 4) ^ CRC32_NIBBLE[value & 0x0FU];
    value = (value >> 4) ^ CRC32_NIBBLE[value & 0x0FU];
  }
  return ~value;
}

bool isValidUtf8(const std::string_view text) {
  for (size_t i = 0; i < text.size();) {
    const uint8_t first = static_cast<uint8_t>(text[i]);
    if (first <= 0x7F) {
      if (first == 0) return false;
      ++i;
      continue;
    }

    size_t continuationCount = 0;
    uint32_t codePoint = 0;
    uint32_t minimum = 0;
    if (first >= 0xC2 && first <= 0xDF) {
      continuationCount = 1;
      codePoint = first & 0x1FU;
      minimum = 0x80;
    } else if (first >= 0xE0 && first <= 0xEF) {
      continuationCount = 2;
      codePoint = first & 0x0FU;
      minimum = 0x800;
    } else if (first >= 0xF0 && first <= 0xF4) {
      continuationCount = 3;
      codePoint = first & 0x07U;
      minimum = 0x10000;
    } else {
      return false;
    }
    if (continuationCount > text.size() - i - 1) return false;
    for (size_t j = 1; j <= continuationCount; ++j) {
      const uint8_t continuation = static_cast<uint8_t>(text[i + j]);
      if ((continuation & 0xC0U) != 0x80U) return false;
      codePoint = (codePoint << 6) | (continuation & 0x3FU);
    }
    if (codePoint < minimum || codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF)) return false;
    i += continuationCount + 1;
  }
  return true;
}

std::string filePathForBook(const std::string_view bookPath, const std::string_view bookType) {
  if (bookPath.empty() || !isValidUtf8(bookPath) || !validBookType(bookType)) return {};
  const uint32_t pathChecksum = crc32(reinterpret_cast<const uint8_t*>(bookPath.data()), bookPath.size());
  return std::string(DIRECTORY) + "/" + std::string(bookType) + "_" + std::to_string(pathChecksum) + ".bin";
}

Status encodeBookMetadata(const BookMetadata& metadata, std::vector<uint8_t>& out) {
  Status status = validateString(metadata.title, MAX_BOOK_TITLE_BYTES);
  if (status != Status::Ok) return status;
  status = validateString(metadata.author, MAX_BOOK_AUTHOR_BYTES);
  if (status != Status::Ok) return status;
  status = validateString(metadata.path, MAX_BOOK_PATH_BYTES);
  if (status != Status::Ok) return status;
  if (metadata.path.empty() || !validBookType(metadata.bookType)) return Status::Corrupt;

  out.clear();
  out.reserve(2 + metadata.title.size() + 2 + metadata.author.size() + 2 + metadata.path.size() + 1 +
              metadata.bookType.size());
  const auto appendString16 = [&](const std::string& value) {
    const size_t start = out.size();
    out.resize(start + 2 + value.size());
    writeU16(out.data() + start, static_cast<uint16_t>(value.size()));
    std::copy(value.begin(), value.end(), out.begin() + static_cast<std::ptrdiff_t>(start + 2));
  };
  appendString16(metadata.title);
  appendString16(metadata.author);
  appendString16(metadata.path);
  out.push_back(static_cast<uint8_t>(metadata.bookType.size()));
  out.insert(out.end(), metadata.bookType.begin(), metadata.bookType.end());
  return Status::Ok;
}

Status encodeRecord(const ClippingMetadata& clipping, std::array<uint8_t, RECORD_SIZE>& out) {
  const Status status = validateClipping(clipping);
  if (status != Status::Ok) return status;
  out.fill(0);
  writeU16(out.data(), clipping.spineIndex);
  writeU16(out.data() + 2, clipping.startPage);
  writeU16(out.data() + 4, clipping.endPage);
  writeU16(out.data() + 6, clipping.pageCount);
  writeU16(out.data() + 8, clipping.startWordIndex);
  writeU16(out.data() + 10, clipping.endWordIndex);
  writeU16(out.data() + 12, clipping.wordCount);
  writeU16(out.data() + 14, clipping.paragraphIndex);
  writeU32(out.data() + 16, clipping.timestamp);
  writeU32(out.data() + 20, clipping.textOffset);
  writeU16(out.data() + 24, clipping.textLength);
  writeU16(out.data() + 26, static_cast<uint16_t>(clipping.chapterTitle.size()));
  std::copy(clipping.chapterTitle.begin(), clipping.chapterTitle.end(), out.begin() + 28);
  writeU32(out.data() + 92, clipping.pageFingerprint);
  return Status::Ok;
}

void encodeHeader(const uint16_t count, const uint32_t fileLength, const uint32_t payloadChecksum,
                  const uint32_t metadataLength, const uint32_t recordsOffset, const uint32_t textsOffset,
                  std::array<uint8_t, HEADER_SIZE>& out) {
  out.fill(0);
  std::copy(MAGIC.begin(), MAGIC.end(), out.begin());
  writeU16(out.data() + 4, VERSION);
  writeU16(out.data() + 6, HEADER_SIZE);
  writeU32(out.data() + 8, fileLength);
  writeU32(out.data() + 12, payloadChecksum);
  writeU16(out.data() + 16, count);
  writeU32(out.data() + 20, metadataLength);
  writeU32(out.data() + 24, recordsOffset);
  writeU32(out.data() + 28, textsOffset);
}

Status inspect(const Source& source, Index& out) {
  out = {};
  if (!source.readAt || source.length == 0) return Status::Truncated;
  uint8_t first = 0;
  if (!readExact(source, 0, &first, 1)) return Status::IoError;
  if (first == MAGIC[0]) return inspectCurrent(source, out);
  if (first == 1 || first == 2) return inspectLegacy(source, first, out);
  return Status::BadMagic;
}

Status readText(const Source& source, const ClippingMetadata& clipping, std::string& out) {
  out.clear();
  if (clipping.textLength == 0 || clipping.textLength > MAX_TEXT_BYTES || clipping.textOffset > source.length ||
      clipping.textLength > source.length - clipping.textOffset) {
    return Status::Corrupt;
  }
  out.resize(clipping.textLength);
  if (!readExact(source, clipping.textOffset, reinterpret_cast<uint8_t*>(out.data()), clipping.textLength)) {
    out.clear();
    return Status::IoError;
  }
  if (!isValidUtf8(out)) {
    out.clear();
    return Status::InvalidUtf8;
  }
  return Status::Ok;
}

}  // namespace ClippingCodec
