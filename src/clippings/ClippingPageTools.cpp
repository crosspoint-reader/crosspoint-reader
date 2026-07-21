#include "ClippingPageTools.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>

namespace ClippingPageTools {
namespace {

size_t whitespaceBytesAt(const std::string_view text, const size_t offset) {
  const uint8_t first = static_cast<uint8_t>(text[offset]);
  if (first < 0x80) return std::isspace(first) ? 1 : 0;
  if (first == 0xC2 && offset + 1 < text.size() && static_cast<uint8_t>(text[offset + 1]) == 0xA0) return 2;
  if (first == 0xE2 && offset + 2 < text.size() && static_cast<uint8_t>(text[offset + 1]) == 0x80) {
    const uint8_t last = static_cast<uint8_t>(text[offset + 2]);
    if ((last >= 0x80 && last <= 0x8A) || last == 0xAF) return 3;
  }
  return 0;
}

void addU16(uint32_t& checksum, const uint16_t value) {
  const std::array<uint8_t, 2> bytes{static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8)};
  checksum = ClippingCodec::crc32(bytes.data(), bytes.size(), checksum);
}

void addU32(uint32_t& checksum, const uint32_t value) {
  const std::array<uint8_t, 4> bytes{static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8),
                                     static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24)};
  checksum = ClippingCodec::crc32(bytes.data(), bytes.size(), checksum);
}

void addBlockStyle(uint32_t& checksum, const BlockStyle& style) {
  addU16(checksum, static_cast<uint16_t>(style.alignment));
  addU16(checksum, static_cast<uint16_t>(style.marginTop));
  addU16(checksum, static_cast<uint16_t>(style.marginBottom));
  addU16(checksum, static_cast<uint16_t>(style.marginLeft));
  addU16(checksum, static_cast<uint16_t>(style.marginRight));
  addU16(checksum, static_cast<uint16_t>(style.paddingTop));
  addU16(checksum, static_cast<uint16_t>(style.paddingBottom));
  addU16(checksum, static_cast<uint16_t>(style.paddingLeft));
  addU16(checksum, static_cast<uint16_t>(style.paddingRight));
  addU16(checksum, static_cast<uint16_t>(style.textIndent));
  uint16_t flags = 0;
  flags |= style.textIndentDefined ? 1U << 0 : 0;
  flags |= style.textAlignDefined ? 1U << 1 : 0;
  flags |= style.isRtl ? 1U << 2 : 0;
  flags |= style.directionDefined ? 1U << 3 : 0;
  flags |= style.fromBrElement ? 1U << 4 : 0;
  addU16(checksum, flags);
}

uint32_t fingerprintImpl(const Page& page, const int fontId, const int viewportWidth, const int viewportHeight,
                         const int lineHeight, const int marginLeft, const int marginTop) {
  uint32_t checksum = 0;
  addU32(checksum, static_cast<uint32_t>(fontId));
  addU32(checksum, static_cast<uint32_t>(viewportWidth));
  addU32(checksum, static_cast<uint32_t>(viewportHeight));
  addU32(checksum, static_cast<uint32_t>(lineHeight));
  addU32(checksum, static_cast<uint32_t>(marginLeft));
  addU32(checksum, static_cast<uint32_t>(marginTop));
  addU32(checksum, static_cast<uint32_t>(page.elements.size()));

  for (const auto& element : page.elements) {
    if (!element) {
      addU16(checksum, 0);
      continue;
    }
    addU16(checksum, static_cast<uint16_t>(element->getTag()));
    addU16(checksum, static_cast<uint16_t>(element->xPos));
    addU16(checksum, static_cast<uint16_t>(element->yPos));
    if (element->getTag() == TAG_PageImage) {
      const auto& image = static_cast<const PageImage&>(*element).getImageBlock();
      addU16(checksum, static_cast<uint16_t>(image.getWidth()));
      addU16(checksum, static_cast<uint16_t>(image.getHeight()));
      continue;
    }
    if (element->getTag() != TAG_PageLine) continue;

    const auto& block = static_cast<const PageLine&>(*element).getBlock();
    if (!block || !block->valid()) {
      addU16(checksum, 0);
      continue;
    }

    addU16(checksum, block->wordCount());
    addBlockStyle(checksum, block->getBlockStyle());
    for (uint16_t i = 0; i < block->wordCount(); ++i) {
      const uint16_t length = block->wordTextLen(i);
      addU16(checksum, static_cast<uint16_t>(block->wordXpos(i)));
      addU16(checksum, static_cast<uint16_t>(block->wordStyle(i)));
      addU16(checksum, block->focusBoundary(i));
      addU16(checksum, block->focusSuffixX(i));
      addU16(checksum, length);
      checksum = ClippingCodec::crc32(reinterpret_cast<const uint8_t*>(block->wordText(i)), length, checksum);
    }
  }
  return checksum == 0 ? 1 : checksum;
}

}  // namespace

bool hasVisibleText(const std::string_view text) {
  for (size_t offset = 0; offset < text.size();) {
    const size_t whitespaceBytes = whitespaceBytesAt(text, offset);
    if (whitespaceBytes == 0) return true;
    offset += whitespaceBytes;
  }
  return false;
}

uint32_t fingerprint(const Page& page, const GfxRenderer& renderer, const int fontId, const int marginLeft,
                     const int marginTop) {
  return fingerprintImpl(page, fontId, renderer.getScreenWidth(), renderer.getScreenHeight(),
                         renderer.getLineHeight(fontId), marginLeft, marginTop);
}

// cppcheck-suppress constParameterReference; keep the public drawing API's mutable renderer reference.
void HighlightPlan::draw(GfxRenderer& renderer) const {
  for (size_t i = 0; i < count; ++i) {
    renderer.drawLine(lines[i].left, lines[i].y, lines[i].right, lines[i].y, 2, true);
  }
}

HighlightPlan buildExactHighlightPlan(GfxRenderer& renderer, const Page& page, const int fontId, const int marginLeft,
                                      const int marginTop,
                                      const std::vector<ClippingCodec::ClippingMetadata>& clippings,
                                      const uint16_t spineIndex, const uint16_t pageIndex,
                                      const uint32_t pageFingerprint, const size_t maxHighlights) {
  HighlightPlan plan;
  if (pageFingerprint == 0 || maxHighlights == 0) return plan;

  const size_t scanLimit = std::min(clippings.size(), static_cast<size_t>(ClippingCodec::MAX_CLIPPINGS_PER_BOOK));
  const size_t matchLimit = std::min(maxHighlights, static_cast<size_t>(ClippingCodec::MAX_CLIPPINGS_PER_BOOK));
  if (matchLimit == 0) return plan;

  const int lineHeight = renderer.getLineHeight(fontId);
  if (lineHeight <= 0) return plan;
  uint32_t pageWordIndex = 0;
  for (const auto& element : page.elements) {
    if (!element || element->getTag() != TAG_PageLine) continue;
    const auto& line = static_cast<const PageLine&>(*element);
    const auto& block = line.getBlock();
    if (!block || !block->valid()) continue;

    for (uint16_t i = 0; i < block->wordCount(); ++i) {
      const std::string_view text(block->wordText(i), block->wordTextLen(i));
      if (!hasVisibleText(text)) continue;
      const uint32_t currentWord = pageWordIndex++;
      bool selected = false;
      size_t matchesSeen = 0;
      for (size_t index = 0; index < scanLimit && matchesSeen < matchLimit; ++index) {
        const auto& clipping = clippings[index];
        if (clipping.spineIndex != spineIndex || clipping.startPage != pageIndex || clipping.endPage != pageIndex ||
            clipping.pageFingerprint != pageFingerprint) {
          continue;
        }
        ++matchesSeen;
        if (currentWord >= clipping.startWordIndex && currentWord <= clipping.endWordIndex) {
          selected = true;
          break;
        }
      }
      if (!selected) continue;

      const int left = line.xPos + block->wordXpos(i) + marginLeft;
      int width = renderer.getTextAdvanceX(fontId, block->wordText(i), block->wordStyle(i));
      if (block->focusBoundary(i) > 0) {
        const size_t suffixOffset = std::min<size_t>(block->focusBoundary(i), text.size());
        width = static_cast<int>(block->focusSuffixX(i)) +
                std::max(0, renderer.getTextAdvanceX(fontId, block->wordText(i) + suffixOffset,
                                                     block->wordStyle(i)));
      }
      const int right = std::min(renderer.getScreenWidth(), left + std::max(1, width));
      const int y = std::clamp(line.yPos + marginTop + lineHeight - 2, 0, renderer.getScreenHeight() - 1);
      if (right > left && right > 0 && left < renderer.getScreenWidth()) {
        if (plan.count == plan.lines.size()) {
          plan.truncated = true;
          return plan;
        }
        plan.lines[plan.count++] = {static_cast<int16_t>(std::max(0, left)), static_cast<int16_t>(right - 1),
                                    static_cast<int16_t>(y)};
      }
    }
  }
  return plan;
}

}  // namespace ClippingPageTools
