#include "RecentBookCoverPainter.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>

#include <algorithm>

#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/cover.h"

namespace {
constexpr int kCoverIconSourceSize = 32;

void drawScaledCoverIcon(const GfxRenderer& renderer, int x, int y, int size) {
  if (size <= 0) return;
  constexpr int bytesPerRow = kCoverIconSourceSize / 8;
  for (int destY = 0; destY < size; ++destY) {
    const int sourceY = destY * kCoverIconSourceSize / size;
    for (int destX = 0; destX < size; ++destX) {
      const int sourceX = destX * kCoverIconSourceSize / size;
      const uint8_t rowByte = CoverIcon[sourceY * bytesPerRow + sourceX / 8];
      const bool background = (rowByte >> (7 - (sourceX % 8))) & 0x01;
      if (background) continue;
      renderer.drawPixel(x + size - 1 - destY, y + destX, true);
    }
  }
}
}  // namespace

void drawDefaultRecentCover(const GfxRenderer& renderer, freeink::ui::Rect rect, int placeholderIconSize) {
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
  const freeink::ui::Rect coverRect = rect;
  renderer.drawRect(coverRect.x, coverRect.y, coverRect.width, coverRect.height, true);
  renderer.fillRect(coverRect.x, coverRect.y + coverRect.height / 3, coverRect.width, 2 * coverRect.height / 3, true);
  const int whiteBandHeight = std::max(1, coverRect.height / 3);
  const int maxIconSize = std::max(1, std::min({coverRect.width - 12, whiteBandHeight - 4, coverRect.height - 12}));
  const int iconSize = std::min(placeholderIconSize > 0 ? placeholderIconSize : 32, maxIconSize);
  drawScaledCoverIcon(renderer, coverRect.x + std::max(0, (coverRect.width - iconSize) / 2),
                      coverRect.y + std::max(0, (whiteBandHeight - iconSize) / 2), iconSize);
}

bool paintRecentBookCoverByIndex(freeink::ui::Rect rect, int bookIndex, void* userData) {
  auto* data = static_cast<RecentBookCoverPainterData*>(userData);
  if (data == nullptr || data->renderer == nullptr || data->books == nullptr) return false;
  if (bookIndex < 0 || bookIndex >= static_cast<int>(data->books->size())) return false;

  const RecentBook& book = (*data->books)[bookIndex];
  if (book.coverBmpPath.empty()) {
    drawDefaultRecentCover(*data->renderer, rect, data->placeholderIconSize);
    return true;
  }

  const int thumbHeight = data->coverHeight > 0 ? data->coverHeight : rect.height;
  const std::string coverBmpPath = UITheme::getCoverThumbPath(book.coverBmpPath, thumbHeight);
  HalFile file;
  if (!Storage.openFileForRead("HOME", coverBmpPath, file)) {
    drawDefaultRecentCover(*data->renderer, rect, data->placeholderIconSize);
    return true;
  }

  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) {
    drawDefaultRecentCover(*data->renderer, rect, data->placeholderIconSize);
    return true;
  }

  data->renderer->fillRect(rect.x, rect.y, rect.width, rect.height, false);
  float cropX = 0.0f;
  float cropY = 0.0f;
  const float bitmapAspect = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
  const float targetAspect = static_cast<float>(rect.width) / static_cast<float>(rect.height);
  if (bitmapAspect > targetAspect) {
    cropX = std::max(0.0f, 1.0f - targetAspect / bitmapAspect);
  } else if (bitmapAspect < targetAspect) {
    cropY = std::max(0.0f, 1.0f - bitmapAspect / targetAspect);
  }
  data->renderer->drawBitmap(bitmap, rect.x, rect.y, rect.width, rect.height, cropX, cropY);
  return true;
}

bool paintRecentCoverGridCover(freeink::ui::DrawTarget&, freeink::ui::Rect rect, const freeink::ui::CoverGridItem& item,
                               uint16_t, void* userData) {
  return paintRecentBookCoverByIndex(rect, item.actionValue, userData);
}

bool paintBookCardCover(freeink::ui::DrawTarget&, freeink::ui::Rect rect, const freeink::ui::BookCardProps& props,
                        void* userData) {
  return paintRecentBookCoverByIndex(rect, props.value, userData);
}
