#include "Page.h"

#include <HardwareSerial.h>
#include <Serialization.h>

constexpr uint8_t PAGE_FILE_VERSION = 2;

void PageLine::render(GfxRenderer& renderer, const int fontId) { block->render(renderer, fontId, xPos, yPos); }

void PageLine::serialize(std::ostream& os) {
  serialization::writePod(os, xPos);
  serialization::writePod(os, yPos);

  // serialize TextBlock pointed to by PageLine
  block->serialize(os);
}

std::unique_ptr<PageLine> PageLine::deserialize(std::istream& is) {
  int32_t xPos;
  int32_t yPos;
  serialization::readPod(is, xPos);
  serialization::readPod(is, yPos);

  auto tb = TextBlock::deserialize(is);
  return std::unique_ptr<PageLine>(new PageLine(std::move(tb), xPos, yPos));
}

void Page::render(GfxRenderer& renderer, const int fontId) const {
  for (const auto element : elements) {
    element->render(renderer, fontId);
  }
}

void Page::serialize(std::ostream& os) const {
  serialization::writePod(os, PAGE_FILE_VERSION);

  const uint32_t count = elements.size();
  serialization::writePod(os, count);

  for (const auto& el : elements) {
    // Only PageLine exists currently
    serialization::writePod(os, static_cast<uint8_t>(TAG_PageLine));
    el->serialize(os);
  }
}

std::unique_ptr<Page> Page::deserialize(std::istream& is) {
  uint8_t version;
  serialization::readPod(is, version);
  if (version != PAGE_FILE_VERSION) {
    Serial.printf("[%lu] [PGE] Deserialization failed: Unknown version %u\n", millis(), version);
    return nullptr;
  }

  auto page = std::unique_ptr<Page>(new Page());

  uint32_t count;
  serialization::readPod(is, count);

  for (uint32_t i = 0; i < count; i++) {
    uint8_t tag;
    serialization::readPod(is, tag);

    if (tag == TAG_PageLine) {
      auto pl = PageLine::deserialize(is);
      page->elements.push_back(std::move(pl));
    } else {
      Serial.printf("[%lu] [PGE] Deserialization failed: Unknown tag %u\n", millis(), tag);
      return nullptr;
    }
  }

  return page;
}
