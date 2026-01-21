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
  size_t lineIdx = 0;
  for (auto& element : elements) {
    element->render(renderer, fontId, xOffset, yOffset);

    // After each line renders, validate ALL subsequent lines for corruption
    // This helps identify which line's rendering corrupts future lines
    size_t checkIdx = 0;
    for (const auto& checkElement : elements) {
      if (checkIdx > lineIdx) {
        int corrupt = checkElement->validate("INTER_LINE_CHECK");
        if (corrupt > 0) {
          Serial.printf("[%lu] [PGE] !! CORRUPTION: Rendering line[%u] corrupted line[%u] (%d words)\n",
                        millis(), (uint32_t)lineIdx, (uint32_t)checkIdx, corrupt);
        }
      }
      checkIdx++;
    }
    lineIdx++;
  }
}

int Page::validate(const char* checkpoint) const {
  int totalCorrupt = 0;
  for (const auto& element : elements) {
    totalCorrupt += element->validate(checkpoint);
  }
  if (totalCorrupt > 0) {
    Serial.printf("[%lu] [PGE] Validation @ %s: %d corrupted words found\n", millis(), checkpoint, totalCorrupt);
  }
  return totalCorrupt;
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

  return page;
}
