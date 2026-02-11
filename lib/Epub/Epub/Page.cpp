#include "Page.h"

#include <HardwareSerial.h>
#include <Serialization.h>

void PageLine::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) {
  block->render(renderer, fontId, xPos + xOffset, yPos + yOffset);
}

bool PageLine::serialize(FsFile& file) {
  serialization::writePod(file, xPos);
  serialization::writePod(file, yPos);

  // serialize TextBlock pointed to by PageLine
  return block->serialize(file);
}

std::unique_ptr<PageLine> PageLine::deserialize(FsFile& file) {
  int16_t xPos;
  int16_t yPos;
  serialization::readPod(file, xPos);
  serialization::readPod(file, yPos);

  auto tb = TextBlock::deserialize(file);
  return std::unique_ptr<PageLine>(new PageLine(std::move(tb), xPos, yPos));
}

void Page::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) const {
  for (auto& element : elements) {
    element->render(renderer, fontId, xOffset, yOffset);
  }
}

bool Page::serialize(FsFile& file) const {
  const uint16_t count = elements.size();
  serialization::writePod(file, count);

  for (const auto& el : elements) {
    // Only PageLine exists currently
    serialization::writePod(file, static_cast<uint8_t>(TAG_PageLine));
    if (!el->serialize(file)) {
      return false;
    }
  }

  // Serialize footnotes
  int32_t fCount = footnotes.size();
  serialization::writePod(file, fCount);
  for (const auto& fn : footnotes) {
    if (file.write(fn.number, 24) != 24 || file.write(fn.href, 64) != 64) {
      Serial.printf("[%lu] [PGE] Serialization failed: Write error for footnote\n", millis());
      return false;
    }
    uint8_t isInlineFlag = fn.isInline ? 1 : 0;
    if (file.write(&isInlineFlag, 1) != 1) {
      Serial.printf("[%lu] [PGE] Serialization failed: Write error for footnote flag\n", millis());
      return false;
    }
  }

  return true;
}

std::unique_ptr<Page> Page::deserialize(FsFile& file) {
  auto page = std::unique_ptr<Page>(new Page());

  uint16_t count;
  serialization::readPod(file, count);

  for (uint16_t i = 0; i < count; i++) {
    uint8_t tag;
    serialization::readPod(file, tag);

    if (tag == TAG_PageLine) {
      auto pl = PageLine::deserialize(file);
      page->elements.push_back(std::move(pl));
    } else {
      Serial.printf("[%lu] [PGE] Deserialization failed: Unknown tag %u\n", millis(), tag);
      return nullptr;
    }
  }

  int32_t footnoteCount;
  serialization::readPod(file, footnoteCount);

  // Sanity check: limit footnoteCount to reasonable maximum
  constexpr int32_t MAX_FOOTNOTES_PER_PAGE = 16;
  if (footnoteCount < 0 || footnoteCount > MAX_FOOTNOTES_PER_PAGE) {
    Serial.printf("[%lu] [PGE] Deserialization failed: Invalid footnote count %d (max %d)\n", millis(), footnoteCount,
                  MAX_FOOTNOTES_PER_PAGE);
    return nullptr;
  }

  for (int i = 0; i < footnoteCount; i++) {
    FootnoteEntry entry;

    // Read buffers and check for errors
    size_t numberBytesRead = file.read(entry.number, 24);
    size_t hrefBytesRead = file.read(entry.href, 64);
    uint8_t isInlineFlag = 0;
    size_t flagBytesRead = file.read(&isInlineFlag, 1);

    // Verify all reads succeeded
    if (numberBytesRead != 24 || hrefBytesRead != 64 || flagBytesRead != 1) {
      Serial.printf("[%lu] [PGE] Deserialization failed: Incomplete footnote read at index %d\n", millis(), i);
      return nullptr;
    }

    // Force null-termination to prevent buffer overruns
    entry.number[23] = '\0';
    entry.href[63] = '\0';

    entry.isInline = (isInlineFlag != 0);
    page->footnotes.push_back(entry);
  }

  return page;

  return page;
}
