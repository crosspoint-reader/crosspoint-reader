#pragma once

#include <FreeInkUI.h>

#include <vector>

class GfxRenderer;
struct RecentBook;

struct RecentBookCoverPainterData {
  const GfxRenderer* renderer = nullptr;
  const std::vector<RecentBook>* books = nullptr;
  int coverHeight = 0;
  int placeholderIconSize = 0;
};

void drawDefaultRecentCover(const GfxRenderer& renderer, freeink::ui::Rect rect, int placeholderIconSize = 0);
bool paintRecentBookCoverByIndex(freeink::ui::Rect rect, int bookIndex, void* userData);
bool paintRecentCoverGridCover(freeink::ui::DrawTarget& target, freeink::ui::Rect rect,
                               const freeink::ui::CoverGridItem& item, uint16_t index, void* userData);
bool paintBookCardCover(freeink::ui::DrawTarget& target, freeink::ui::Rect rect,
                        const freeink::ui::BookCardProps& props, void* userData);
