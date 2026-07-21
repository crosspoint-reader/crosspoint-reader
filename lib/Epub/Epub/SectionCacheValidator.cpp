#include "SectionCacheValidator.h"

#include <algorithm>
#include <cmath>

namespace {

template <typename T>
bool readPod(SectionCacheValidation::Input& input, const uint64_t offset, T& value) {
  value = {};
  return offset <= input.size() && sizeof(T) <= input.size() - offset && input.readAt(offset, &value, sizeof(T));
}

class Cursor {
 public:
  Cursor(SectionCacheValidation::Input& input, const uint64_t begin, const uint64_t end)
      : input_(input), position_(begin), end_(end), valid_(begin <= end && end <= input.size()) {}

  template <typename T>
  bool read(T& value) {
    if (!valid_ || sizeof(T) > remaining() || !readPod(input_, position_, value)) {
      valid_ = false;
      value = {};
      return false;
    }
    position_ += sizeof(T);
    return true;
  }

  bool skip(const uint64_t length) {
    if (!valid_ || length > remaining()) {
      valid_ = false;
      return false;
    }
    position_ += length;
    return true;
  }

  uint64_t position() const { return position_; }
  uint64_t end() const { return end_; }
  uint64_t remaining() const { return valid_ ? end_ - position_ : 0; }
  bool atEnd() const { return valid_ && position_ == end_; }
  SectionCacheValidation::Input& input() { return input_; }

 private:
  SectionCacheValidation::Input& input_;
  uint64_t position_ = 0;
  uint64_t end_ = 0;
  bool valid_ = false;
};

bool readSerializedBool(Cursor& cursor) {
  uint8_t value = 0;
  return cursor.read(value) && value <= 1;
}

bool consumeRange(SectionCacheValidation::Input& input, const uint64_t begin, const uint64_t length) {
  uint8_t buffer[128];
  uint64_t consumed = 0;
  while (consumed < length) {
    const size_t chunk =
        static_cast<size_t>(std::min<uint64_t>(sizeof(buffer), static_cast<uint64_t>(length) - consumed));
    if (!input.readAt(begin + consumed, buffer, chunk)) return false;
    consumed += chunk;
  }
  return true;
}

bool consume(Cursor& cursor, const uint64_t length) {
  return length <= cursor.remaining() && consumeRange(cursor.input(), cursor.position(), length) && cursor.skip(length);
}

bool validateTextBlock(Cursor& cursor) {
  uint16_t wordCount = 0;
  uint8_t hasFocus = 0;
  uint16_t textBytes = 0;
  if (!cursor.read(wordCount) || !cursor.read(hasFocus) || !cursor.read(textBytes) || hasFocus > 1 ||
      wordCount > SectionCacheValidation::MAX_TEXT_BLOCK_WORDS ||
      (wordCount == 0 && (hasFocus != 0 || textBytes != 0)) || (wordCount > 0 && textBytes < wordCount)) {
    return false;
  }

  const uint64_t arenaStart = cursor.position();
  const uint64_t wordBytes = static_cast<uint64_t>(wordCount);
  const uint64_t textStart = arenaStart + wordBytes * (5U + (hasFocus != 0 ? 3U : 0U));
  const uint64_t arenaEnd = textStart + textBytes;
  if (textStart < arenaStart || arenaEnd < textStart || arenaEnd > cursor.end()) return false;

  if (wordCount > 0) {
    const uint64_t styleStart = arenaStart + wordBytes * 4U + (hasFocus != 0 ? wordBytes * 2U : 0U);
    const uint64_t focusBoundaryStart = styleStart + wordBytes;
    uint16_t previousOffset = 0;
    for (uint16_t word = 0; word < wordCount; ++word) {
      uint16_t offset = 0;
      uint8_t precedingByte = 0;
      if (!readPod(cursor.input(), arenaStart + static_cast<uint64_t>(word) * sizeof(uint16_t), offset) ||
          (word == 0 ? offset != 0
                     : offset <= previousOffset ||
                           !readPod(cursor.input(), textStart + static_cast<uint64_t>(offset) - 1U, precedingByte) ||
                           precedingByte != 0) ||
          offset >= textBytes) {
        return false;
      }
      previousOffset = offset;
    }

    const uint64_t ignoredArraysStart = arenaStart + wordBytes * sizeof(uint16_t);
    const uint64_t ignoredArraysBytes = wordBytes * (sizeof(int16_t) + (hasFocus != 0 ? sizeof(uint16_t) : 0U));
    if (!consumeRange(cursor.input(), ignoredArraysStart, ignoredArraysBytes)) return false;

    for (uint16_t word = 0; word < wordCount; ++word) {
      uint8_t style = 0;
      if (!readPod(cursor.input(), styleStart + word, style) || (style & ~0x3FU) != 0) return false;
    }
    if (hasFocus != 0) {
      for (uint16_t word = 0; word < wordCount; ++word) {
        uint8_t boundary = 0;
        if (!readPod(cursor.input(), focusBoundaryStart + word, boundary) || boundary > 36U) return false;
      }
    }

    uint32_t terminators = 0;
    uint64_t textOffset = 0;
    uint8_t lastByte = 0;
    uint8_t buffer[128];
    while (textOffset < textBytes) {
      const size_t chunk =
          static_cast<size_t>(std::min<uint64_t>(sizeof(buffer), static_cast<uint64_t>(textBytes) - textOffset));
      if (!cursor.input().readAt(textStart + textOffset, buffer, chunk)) return false;
      for (size_t byte = 0; byte < chunk; ++byte) terminators += buffer[byte] == 0;
      lastByte = buffer[chunk - 1];
      textOffset += chunk;
    }
    if (terminators != wordCount || lastByte != 0) return false;
  }

  if (!cursor.skip(arenaEnd - arenaStart)) return false;

  uint8_t alignment = 0;
  if (!cursor.read(alignment) || alignment > 4 || !readSerializedBool(cursor)) return false;
  int16_t spacing = 0;
  for (uint8_t field = 0; field < 9; ++field) {
    if (!cursor.read(spacing)) return false;
  }
  return readSerializedBool(cursor) && readSerializedBool(cursor) && readSerializedBool(cursor);
}

bool validatePage(SectionCacheValidation::Input& input, const uint64_t begin, const uint64_t end) {
  Cursor cursor(input, begin, end);
  uint16_t elementCount = 0;
  if (!cursor.read(elementCount) || elementCount > SectionCacheValidation::MAX_PAGE_ELEMENTS) return false;

  for (uint16_t element = 0; element < elementCount; ++element) {
    uint8_t tag = 0;
    int16_t coordinate = 0;
    if (!cursor.read(tag) || !cursor.read(coordinate) || !cursor.read(coordinate)) return false;
    if (tag == 1) {
      if (!validateTextBlock(cursor)) return false;
    } else if (tag == 2) {
      uint32_t pathLength = 0;
      int16_t width = 0;
      int16_t height = 0;
      if (!cursor.read(pathLength) || pathLength == 0 || pathLength > SectionCacheValidation::MAX_IMAGE_PATH_BYTES ||
          !consume(cursor, pathLength) || !cursor.read(width) || !cursor.read(height) || width <= 0 || height <= 0) {
        return false;
      }
    } else if (tag == 3) {
      uint16_t width = 0;
      uint8_t thickness = 0;
      if (!cursor.read(width) || !cursor.read(thickness) || width == 0 || thickness == 0) return false;
    } else {
      return false;
    }
  }

  uint16_t footnoteCount = 0;
  return cursor.read(footnoteCount) && footnoteCount <= SectionCacheValidation::MAX_FOOTNOTES_PER_PAGE &&
         consume(cursor, static_cast<uint64_t>(footnoteCount) * SectionCacheValidation::FOOTNOTE_BYTES) &&
         cursor.atEnd();
}

bool validateHeader(SectionCacheValidation::Input& input, const uint64_t headerSize, const uint8_t finalizedVersion,
                    const uint8_t partialVersion, SectionCacheValidation::Layout& layout) {
  if (headerSize == 0 || input.size() < headerSize) return false;
  Cursor cursor(input, 0, headerSize);
  if (!cursor.read(layout.version) || (layout.version != finalizedVersion && layout.version != partialVersion)) {
    return false;
  }
  layout.partial = layout.version == partialVersion;

  int32_t fontId = 0;
  float lineCompression = 0;
  uint8_t paragraphAlignment = 0;
  uint16_t viewportWidth = 0;
  uint16_t viewportHeight = 0;
  uint8_t imageRendering = 0;
  if (!cursor.read(fontId) || !cursor.read(lineCompression) || !std::isfinite(lineCompression) ||
      lineCompression <= 0 || !readSerializedBool(cursor) || !cursor.read(paragraphAlignment) ||
      paragraphAlignment > 4 || !cursor.read(viewportWidth) || !cursor.read(viewportHeight) || viewportWidth == 0 ||
      viewportHeight == 0 || !readSerializedBool(cursor) || !readSerializedBool(cursor) ||
      !cursor.read(imageRendering) || imageRendering > 2 || !readSerializedBool(cursor) ||
      !cursor.read(layout.pageCount) || !cursor.read(layout.pageLutOffset) || !cursor.read(layout.anchorMapOffset) ||
      !cursor.read(layout.paragraphLutOffset) || !cursor.read(layout.listItemLutOffset) || !cursor.atEnd()) {
    return false;
  }
  (void)fontId;
  return !layout.partial || layout.pageCount > 0;
}

bool validateTables(SectionCacheValidation::Input& input, const uint64_t headerSize,
                    SectionCacheValidation::Layout& layout) {
  const uint64_t fileSize = input.size();
  const uint64_t pageLutEnd =
      static_cast<uint64_t>(layout.pageLutOffset) + static_cast<uint64_t>(layout.pageCount) * sizeof(uint32_t);
  const uint64_t paragraphEnd = static_cast<uint64_t>(layout.paragraphLutOffset) + sizeof(uint16_t) +
                                static_cast<uint64_t>(layout.pageCount) * sizeof(uint16_t);
  const uint64_t listItemEnd =
      static_cast<uint64_t>(layout.listItemLutOffset) + static_cast<uint64_t>(layout.pageCount) * sizeof(uint16_t);
  const uint64_t expectedEnd = listItemEnd + (layout.partial ? 2U * sizeof(uint32_t) : 0U);
  if (layout.pageLutOffset < headerSize || (layout.pageCount == 0 && layout.pageLutOffset != headerSize) ||
      pageLutEnd != layout.anchorMapOffset ||
      static_cast<uint64_t>(layout.anchorMapOffset) + sizeof(uint16_t) > layout.paragraphLutOffset ||
      paragraphEnd != layout.listItemLutOffset || expectedEnd != fileSize) {
    return false;
  }

  Cursor anchors(input, layout.anchorMapOffset, layout.paragraphLutOffset);
  uint16_t anchorCount = 0;
  if (!anchors.read(anchorCount)) return false;
  for (uint16_t anchor = 0; anchor < anchorCount; ++anchor) {
    uint32_t length = 0;
    uint16_t page = 0;
    if (!anchors.read(length) || length == 0 || length > SectionCacheValidation::MAX_ANCHOR_BYTES ||
        length > anchors.remaining() || !consume(anchors, length) || !anchors.read(page) || page >= layout.pageCount) {
      return false;
    }
  }
  if (!anchors.atEnd()) return false;

  Cursor paragraph(input, layout.paragraphLutOffset, layout.listItemLutOffset);
  uint16_t paragraphCount = 0;
  if (!paragraph.read(paragraphCount) || paragraphCount != layout.pageCount ||
      !consume(paragraph, static_cast<uint64_t>(paragraphCount) * sizeof(uint16_t)) || !paragraph.atEnd()) {
    return false;
  }

  Cursor listItems(input, layout.listItemLutOffset, listItemEnd);
  if (!consume(listItems, static_cast<uint64_t>(layout.pageCount) * sizeof(uint16_t)) || !listItems.atEnd()) {
    return false;
  }

  if (layout.partial) {
    Cursor trailer(input, listItemEnd, fileSize);
    if (!trailer.read(layout.partialBytesConsumed) || !trailer.read(layout.partialTotalBytes) || !trailer.atEnd() ||
        layout.partialBytesConsumed == 0 || layout.partialTotalBytes < layout.partialBytesConsumed) {
      return false;
    }
  }
  return true;
}

}  // namespace

namespace SectionCacheValidation {

bool pageBounds(Input& input, const Layout& layout, const uint16_t page, uint64_t& begin, uint64_t& end) {
  begin = 0;
  end = 0;
  if (input.size() != layout.fileSize || page >= layout.pageCount) return false;
  const uint64_t entryOffset =
      static_cast<uint64_t>(layout.pageLutOffset) + static_cast<uint64_t>(page) * sizeof(uint32_t);
  uint32_t pageBegin = 0;
  uint32_t pageEnd = layout.pageLutOffset;
  if (!readPod(input, entryOffset, pageBegin) ||
      (page + 1U < layout.pageCount && !readPod(input, entryOffset + sizeof(uint32_t), pageEnd)) ||
      pageBegin < layout.headerSize || pageBegin >= pageEnd || pageEnd > layout.pageLutOffset) {
    return false;
  }
  begin = pageBegin;
  end = pageEnd;
  return true;
}

bool validate(Input& input, const uint64_t headerSize, const uint8_t finalizedVersion, const uint8_t partialVersion,
              Layout& layout) {
  layout = {};
  layout.headerSize = headerSize;
  layout.fileSize = input.size();
  if (finalizedVersion == partialVersion ||
      !validateHeader(input, headerSize, finalizedVersion, partialVersion, layout) ||
      !validateTables(input, headerSize, layout)) {
    layout = {};
    return false;
  }

  constexpr uint16_t PAGE_LUT_CHUNK = 64;
  uint32_t positions[PAGE_LUT_CHUNK + 1]{};
  for (uint32_t firstPage = 0; firstPage < layout.pageCount; firstPage += PAGE_LUT_CHUNK) {
    const uint16_t pagesInChunk =
        static_cast<uint16_t>(std::min<uint32_t>(PAGE_LUT_CHUNK, static_cast<uint32_t>(layout.pageCount) - firstPage));
    const bool hasFollowingPage = firstPage + pagesInChunk < layout.pageCount;
    const size_t positionsToRead = pagesInChunk + (hasFollowingPage ? 1U : 0U);
    const uint64_t lutReadOffset =
        static_cast<uint64_t>(layout.pageLutOffset) + static_cast<uint64_t>(firstPage) * sizeof(uint32_t);
    if (!input.readAt(lutReadOffset, positions, positionsToRead * sizeof(uint32_t))) {
      layout = {};
      return false;
    }
    for (uint16_t index = 0; index < pagesInChunk; ++index) {
      const uint32_t page = firstPage + index;
      const uint64_t pageBegin = positions[index];
      const uint64_t pageEnd = index + 1U < positionsToRead ? positions[index + 1U] : layout.pageLutOffset;
      if (pageBegin < headerSize || pageBegin >= pageEnd || pageEnd > layout.pageLutOffset ||
          (page == 0 && pageBegin != headerSize) || !validatePage(input, pageBegin, pageEnd)) {
        layout = {};
        return false;
      }
    }
  }
  return true;
}

}  // namespace SectionCacheValidation
