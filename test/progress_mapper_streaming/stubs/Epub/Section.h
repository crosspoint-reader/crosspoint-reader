#pragma once

#include "Epub.h"
#include "GfxRenderer.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

class Section {
 public:
  Section(const std::shared_ptr<Epub>&, int, GfxRenderer&) {}
  std::optional<std::uint16_t> getCachedPageCount() const { return 8; }
  std::optional<std::uint16_t> getPageForVisibleTextOffset(std::uint32_t, bool = false) const { return std::nullopt; }
  std::optional<std::uint16_t> getPageForListItemIndex(std::uint16_t) const { return std::nullopt; }
  std::optional<std::uint16_t> getPageForAnchor(const std::string&) const { return std::nullopt; }
  std::optional<std::uint16_t> getPageForParagraphIndex(std::uint16_t) const { return std::nullopt; }
};
