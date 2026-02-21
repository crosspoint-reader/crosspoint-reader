#include "Page.h"

#include <Logging.h>
#include <Serialization.h>

void PageLine::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) {
  block->render(renderer, fontId, xPos + xOffset, yPos + yOffset);
}

void PageLine::collectText(std::string& out) const {
  for (const auto& word : block->getWords()) {
    out += word;
  }
}

void PageLine::countStyleChars(uint32_t counts[4]) const {
  auto wIt = block->getWords().cbegin();
  auto sIt = block->getWordStyles().cbegin();
  while (wIt != block->getWords().cend() && sIt != block->getWordStyles().cend()) {
    const uint8_t s = static_cast<uint8_t>(*sIt);
    if (s < 4) {
      // Count UTF-8 codepoints (non-continuation bytes) to avoid overweighting multibyte scripts
      uint32_t cpCount = 0;
      for (unsigned char c : *wIt) {
        if ((c & 0xC0) != 0x80) cpCount++;
      }
      counts[s] += cpCount;
    }
    ++wIt;
    ++sIt;
  }
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

void PageImage::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) {
  // Images don't use fontId or text rendering
  imageBlock->render(renderer, xPos + xOffset, yPos + yOffset);
}

bool PageImage::serialize(FsFile& file) {
  serialization::writePod(file, xPos);
  serialization::writePod(file, yPos);

  // serialize ImageBlock
  return imageBlock->serialize(file);
}

std::unique_ptr<PageImage> PageImage::deserialize(FsFile& file) {
  int16_t xPos;
  int16_t yPos;
  serialization::readPod(file, xPos);
  serialization::readPod(file, yPos);

  auto ib = ImageBlock::deserialize(file);
  return std::unique_ptr<PageImage>(new PageImage(std::move(ib), xPos, yPos));
}

void Page::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) const {
  for (auto& element : elements) {
    element->render(renderer, fontId, xOffset, yOffset);
  }
}

void Page::collectText(std::string& out) const {
  for (const auto& element : elements) {
    element->collectText(out);
  }
}

EpdFontFamily::Style Page::getDominantStyle() const {
  uint32_t counts[4] = {};
  for (const auto& element : elements) {
    element->countStyleChars(counts);
  }
  uint8_t best = 0;
  for (uint8_t i = 1; i < 4; i++) {
    if (counts[i] > counts[best]) best = i;
  }
  return static_cast<EpdFontFamily::Style>(best);
}

bool Page::serialize(FsFile& file) const {
  const uint16_t count = elements.size();
  serialization::writePod(file, count);

  for (const auto& el : elements) {
    // Use getTag() method to determine type
    serialization::writePod(file, static_cast<uint8_t>(el->getTag()));

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
    } else if (tag == TAG_PageImage) {
      auto pi = PageImage::deserialize(file);
      page->elements.push_back(std::move(pi));
    } else {
      LOG_ERR("PGE", "Deserialization failed: Unknown tag %u", tag);
      return nullptr;
    }
  }

  return page;
}
