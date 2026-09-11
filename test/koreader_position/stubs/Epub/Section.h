#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

class Epub;
class GfxRenderer;

// These tests exercise content anchors without a device-specific page cache.
class Section {
 public:
  Section(const std::shared_ptr<Epub>&, int, GfxRenderer&) {}
  std::optional<uint16_t> getCachedPageCount() const { return std::nullopt; }
  std::optional<uint16_t> getPageForAnchor(const std::string&) const { return std::nullopt; }
  std::optional<uint16_t> getPageForParagraphIndex(uint16_t) const { return std::nullopt; }
  std::optional<uint16_t> getPageForListItemIndex(uint16_t) const { return std::nullopt; }
  std::optional<uint16_t> getPageForVisibleTextOffset(uint32_t, bool = false) const { return std::nullopt; }
};
