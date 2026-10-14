#include "Page.h"

#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>

namespace {

template <typename Predicate>
void renderFilteredPageElements(const std::vector<std::unique_ptr<PageElement>>& elements, GfxRenderer& renderer,
                                const int fontId, const int xOffset, const int yOffset, Predicate&& predicate) {
  for (const auto& element : elements) {
    if (predicate(*element)) {
      element->render(renderer, fontId, xOffset, yOffset);
    }
  }
}

}  // namespace

void PageLine::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) {
  block->render(renderer, fontId, xPos + xOffset, yPos + yOffset);
}

bool PageLine::serialize(HalFile& file) {
  if (!(serialization::tryWritePod(file, xPos) && serialization::tryWritePod(file, yPos))) {
    LOG_ERR("CACHE", "Incomplete cache data");
    return false;
  }

  // serialize TextBlock pointed to by PageLine
  return block->serialize(file);
}

std::unique_ptr<PageLine> PageLine::deserialize(HalFile& file) {
  int16_t xPos;
  int16_t yPos;
  if (!(serialization::tryReadPod(file, xPos) && serialization::tryReadPod(file, yPos))) {
    LOG_ERR("CACHE", "Incomplete cache data");
    return nullptr;
  }

  auto tb = TextBlock::deserialize(file);
  if (!tb) {
    LOG_ERR("PGE", "Deserialization failed: null TextBlock");
    return nullptr;
  }

  auto line = makeUniqueNoThrow<PageLine>(std::move(tb), xPos, yPos);
  if (!line) {
    LOG_ERR("PGE", "Deserialization failed: could not allocate PageLine");
    return nullptr;
  }
  return line;
}

void PageImage::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) {
  // Images don't use fontId or text rendering
  imageBlock->render(renderer, xPos + xOffset, yPos + yOffset);
}

void PageImage::renderPlaceholder(GfxRenderer& renderer, const int xOffset, const int yOffset) const {
  imageBlock->renderPlaceholder(renderer, xPos + xOffset, yPos + yOffset);
}

bool PageImage::serialize(HalFile& file) {
  if (!(serialization::tryWritePod(file, xPos) && serialization::tryWritePod(file, yPos))) {
    LOG_ERR("CACHE", "Incomplete cache data");
    return false;
  }

  // serialize ImageBlock
  return imageBlock->serialize(file);
}

std::unique_ptr<PageImage> PageImage::deserialize(HalFile& file) {
  int16_t xPos;
  int16_t yPos;
  if (!(serialization::tryReadPod(file, xPos) && serialization::tryReadPod(file, yPos))) {
    LOG_ERR("CACHE", "Incomplete cache data");
    return nullptr;
  }

  auto ib = ImageBlock::deserialize(file);
  if (!ib) {
    LOG_ERR("PGE", "Deserialization failed: null ImageBlock");
    return nullptr;
  }
  auto image = makeUniqueNoThrow<PageImage>(std::move(ib), xPos, yPos);
  if (!image) {
    LOG_ERR("PGE", "Deserialization failed: could not allocate PageImage");
    return nullptr;
  }
  return image;
}

void PageHorizontalRule::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) {
  (void)fontId;
  if (width == 0 || thickness == 0) {
    return;
  }

  renderer.drawLine(xPos + xOffset, yPos + yOffset, xPos + xOffset + width - 1, yPos + yOffset, thickness, true);
}

bool PageHorizontalRule::serialize(HalFile& file) {
  if (!(serialization::tryWritePod(file, xPos) && serialization::tryWritePod(file, yPos) &&
        serialization::tryWritePod(file, width) && serialization::tryWritePod(file, thickness))) {
    LOG_ERR("CACHE", "Incomplete cache data");
    return false;
  }
  return true;
}

std::unique_ptr<PageHorizontalRule> PageHorizontalRule::deserialize(HalFile& file) {
  int16_t xPos = 0;
  int16_t yPos = 0;
  uint16_t width = 0;
  uint8_t thickness = 0;
  if (!(serialization::tryReadPod(file, xPos) && serialization::tryReadPod(file, yPos) &&
        serialization::tryReadPod(file, width) && serialization::tryReadPod(file, thickness))) {
    LOG_ERR("CACHE", "Incomplete cache data");
    return nullptr;
  }

  if (width == 0 || thickness == 0) {
    LOG_ERR("PGE", "Deserialization failed: invalid horizontal rule metadata (width=%u thickness=%u)", width,
            thickness);
    return nullptr;
  }

  auto rule = makeUniqueNoThrow<PageHorizontalRule>(width, thickness, xPos, yPos);
  if (!rule) {
    LOG_ERR("PGE", "Deserialization failed: could not allocate PageHorizontalRule");
    return nullptr;
  }
  return rule;
}

void Page::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) const {
  renderFilteredPageElements(elements, renderer, fontId, xOffset, yOffset, [](const PageElement&) { return true; });
}

void Page::renderImages(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) const {
  renderFilteredPageElements(elements, renderer, fontId, xOffset, yOffset,
                             [](const PageElement& element) { return element.getTag() == TAG_PageImage; });
}

void Page::renderWithImagePlaceholders(GfxRenderer& renderer, const int fontId, const int xOffset,
                                       const int yOffset) const {
  for (const auto& element : elements) {
    if (element->getTag() == TAG_PageImage) {
      static_cast<const PageImage&>(*element).renderPlaceholder(renderer, xOffset, yOffset);
    } else {
      element->render(renderer, fontId, xOffset, yOffset);
    }
  }
}

bool Page::serialize(HalFile& file) const {
  const uint16_t count = elements.size();
  if (!serialization::tryWritePod(file, count)) {
    LOG_ERR("CACHE", "Failed cache writePod");
    return false;
  }

  for (const auto& el : elements) {
    // Use getTag() method to determine type
    if (!serialization::tryWritePod(file, static_cast<uint8_t>(el->getTag()))) {
      LOG_ERR("CACHE", "Failed cache writePod");
      return false;
    }

    if (!el->serialize(file)) {
      return false;
    }
  }

  // Serialize footnotes (clamp to MAX_FOOTNOTES_PER_PAGE to match addFootnote/deserialize limits)
  const uint16_t fnCount = std::min<uint16_t>(footnotes.size(), MAX_FOOTNOTES_PER_PAGE);
  if (!serialization::tryWritePod(file, fnCount)) {
    LOG_ERR("CACHE", "Failed cache writePod");
    return false;
  }
  for (uint16_t i = 0; i < fnCount; i++) {
    const auto& fn = footnotes[i];
    if (file.write(fn.number, sizeof(fn.number)) != sizeof(fn.number) ||
        file.write(fn.href, sizeof(fn.href)) != sizeof(fn.href)) {
      LOG_ERR("PGE", "Failed to write footnote");
      return false;
    }
  }

  const uint16_t linkCount = std::min<uint16_t>(links.size(), MAX_LINKS_PER_PAGE);
  if (!serialization::tryWritePod(file, linkCount)) {
    LOG_ERR("CACHE", "Failed cache writePod");
    return false;
  }
  for (uint16_t i = 0; i < linkCount; i++) {
    const auto& link = links[i];
    if (file.write(link.href, sizeof(link.href)) != sizeof(link.href)) {
      LOG_ERR("PGE", "Failed to write link %u", i);
      return false;
    }
    if (!(serialization::tryWritePod(file, link.x) && serialization::tryWritePod(file, link.y) &&
          serialization::tryWritePod(file, link.width) && serialization::tryWritePod(file, link.height))) {
      LOG_ERR("CACHE", "Incomplete cache data");
      return false;
    }
  }

  return true;
}

std::unique_ptr<Page> Page::deserialize(HalFile& file) {
  auto page = makeUniqueNoThrow<Page>();
  if (!page) {
    LOG_ERR("PGE", "Deserialization failed: could not allocate Page");
    return nullptr;
  }

  uint16_t count;
  if (!serialization::tryReadPod(file, count)) {
    LOG_ERR("CACHE", "Failed cache readPod");
    return nullptr;
  }

  // Reserve up front so a page load costs one allocation for the element vector
  // instead of a grow-copy-free cycle every doubling. `count` is untrusted (it
  // comes straight off the SD cache), so clamp it: a real page holds a few dozen
  // elements, while a corrupt header could ask for 65535 * sizeof(unique_ptr) and
  // abort() on the failed allocation (vector's operator new is throwing, and this
  // firmware builds with -fno-exceptions). Under-reserving is harmless -- the
  // push_back path below still grows normally.
  static constexpr uint16_t RESERVE_CAP = 256;
  page->elements.reserve(std::min(count, RESERVE_CAP));

  for (uint16_t i = 0; i < count; i++) {
    uint8_t tag;
    if (!serialization::tryReadPod(file, tag)) {
      LOG_ERR("CACHE", "Failed cache readPod");
      return nullptr;
    }

    if (tag == TAG_PageLine) {
      auto pl = PageLine::deserialize(file);
      if (!pl) {
        return nullptr;
      }
      page->elements.push_back(std::move(pl));
    } else if (tag == TAG_PageImage) {
      auto pi = PageImage::deserialize(file);
      if (!pi) {
        return nullptr;
      }
      page->elements.push_back(std::move(pi));
    } else if (tag == TAG_PageHorizontalRule) {
      auto rule = PageHorizontalRule::deserialize(file);
      if (!rule) {
        return nullptr;
      }
      page->elements.push_back(std::move(rule));
    } else {
      LOG_ERR("PGE", "Deserialization failed: Unknown tag %u", tag);
      return nullptr;
    }
  }

  // Deserialize footnotes
  uint16_t fnCount;
  if (!serialization::tryReadPod(file, fnCount)) {
    LOG_ERR("CACHE", "Failed cache readPod");
    return nullptr;
  }
  if (fnCount > MAX_FOOTNOTES_PER_PAGE) {
    LOG_ERR("PGE", "Invalid footnote count %u", fnCount);
    return nullptr;
  }
  page->footnotes.resize(fnCount);
  for (uint16_t i = 0; i < fnCount; i++) {
    auto& entry = page->footnotes[i];
    if (file.read(entry.number, sizeof(entry.number)) != sizeof(entry.number) ||
        file.read(entry.href, sizeof(entry.href)) != sizeof(entry.href)) {
      LOG_ERR("PGE", "Failed to read footnote %u", i);
      return nullptr;
    }
    entry.number[sizeof(entry.number) - 1] = '\0';
    entry.href[sizeof(entry.href) - 1] = '\0';
  }

  uint16_t linkCount;
  if (!serialization::tryReadPod(file, linkCount)) {
    LOG_ERR("CACHE", "Failed cache readPod");
    return nullptr;
  }
  if (linkCount > MAX_LINKS_PER_PAGE) {
    LOG_ERR("PGE", "Invalid link count %u", linkCount);
    return nullptr;
  }
  page->links.resize(linkCount);
  for (uint16_t i = 0; i < linkCount; i++) {
    auto& link = page->links[i];
    if (file.read(link.href, sizeof(link.href)) != sizeof(link.href)) {
      LOG_ERR("PGE", "Failed to read link %u", i);
      return nullptr;
    }
    link.href[sizeof(link.href) - 1] = '\0';
    if (!(serialization::tryReadPod(file, link.x) && serialization::tryReadPod(file, link.y) &&
          serialization::tryReadPod(file, link.width) && serialization::tryReadPod(file, link.height))) {
      LOG_ERR("CACHE", "Incomplete cache data");
      return nullptr;
    }
    if (link.href[0] == '\0' || link.width <= 0 || link.height <= 0) {
      LOG_ERR("PGE", "Invalid link geometry %u", i);
      return nullptr;
    }
  }

  return page;
}
