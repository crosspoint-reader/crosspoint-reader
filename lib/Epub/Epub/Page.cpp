#include "Page.h"

#include <HardwareSerial.h>
#include <Serialization.h>

namespace {
constexpr uint8_t PAGE_FILE_VERSION = 3;
}

void PageLine::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) {
  block->render(renderer, fontId, xPos + xOffset, yPos + yOffset);
}

void PageLine::serialize(File& file) {
  serialization::writePod(file, xPos);
  serialization::writePod(file, yPos);

  // serialize TextBlock pointed to by PageLine
  block->serialize(file);
}

std::unique_ptr<PageLine> PageLine::deserialize(File& file) {
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

void Page::serialize(File& file) const {
  serialization::writePod(file, PAGE_FILE_VERSION);

  const uint32_t count = elements.size();
  serialization::writePod(file, count);

  for (const auto& el : elements) {
    // Only PageLine exists currently
    serialization::writePod(file, static_cast<uint8_t>(TAG_PageLine));
    el->serialize(file);
  }
}

std::unique_ptr<Page> Page::deserialize(File& file) {
  uint8_t version;
  serialization::readPod(file, version);
  if (version != PAGE_FILE_VERSION) {
    Serial.printf("[%lu] [PGE] Deserialization failed: Unknown version %u\n", millis(), version);
    return nullptr;
  }

  auto page = std::unique_ptr<Page>(new Page());

  uint32_t count;
  serialization::readPod(file, count);

  for (uint32_t i = 0; i < count; i++) {
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
