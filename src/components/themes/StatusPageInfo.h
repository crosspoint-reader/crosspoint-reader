#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

struct StatusPageInfo {
  uint32_t currentPage = 0;
  uint32_t totalPages = 0;
  bool totalKnown = true;
  bool hasMorePages = false;
  bool futureIndexingActive = false;
};

struct IndexingIndicatorPosition {
  int x = 0;
  int y = 0;
};

using FutureIndexingDotPosition = IndexingIndicatorPosition;

inline constexpr int INDEXING_INDICATOR_GAP = 4;
inline constexpr int INDEXING_INDICATOR_AFTER_PAGE_COUNT_GAP = 5;
inline constexpr int INDEXING_INDICATOR_STANDALONE_RIGHT_INSET = 10;
inline constexpr int INDEXING_INDICATOR_STANDALONE_TEXT_BOTTOM_GAP = 4;
inline constexpr int CURRENT_INDEXING_STANDALONE_PLUS_SIZE = 9;
inline constexpr int CURRENT_INDEXING_STANDALONE_PLUS_STROKE = 2;
inline constexpr int FUTURE_INDEXING_DOT_SIZE = 3;
inline constexpr int FUTURE_INDEXING_DOT_TEXT_Y_NUDGE = 2;

inline constexpr bool shouldDrawChapterProgressBar(const StatusPageInfo& pageInfo) {
  return pageInfo.totalKnown && pageInfo.totalPages > 0;
}

inline constexpr bool shouldDrawCurrentIndexingIndicator(const StatusPageInfo& pageInfo) {
  return !pageInfo.totalKnown && pageInfo.hasMorePages && pageInfo.totalPages > 0;
}

inline constexpr bool shouldDrawFutureIndexingIndicator(const StatusPageInfo& pageInfo) {
  return pageInfo.futureIndexingActive;
}

inline constexpr int indexingIndicatorCenteredOffset(const int outerSize, const int innerSize) {
  return outerSize > innerSize ? (outerSize - innerSize) / 2 : 0;
}

inline constexpr IndexingIndicatorPosition currentIndexingStandalonePlusBottomRight(
    const int screenWidth, const int screenHeight, const int orientedMarginRight, const int orientedMarginBottom,
    const int paddingBottom, const int reservedBottomHeight, const int statusBarVerticalMargin, const int textHeight) {
  return IndexingIndicatorPosition{
      screenWidth - orientedMarginRight - INDEXING_INDICATOR_STANDALONE_RIGHT_INSET -
          CURRENT_INDEXING_STANDALONE_PLUS_SIZE,
      screenHeight - orientedMarginBottom - paddingBottom - reservedBottomHeight - statusBarVerticalMargin -
          INDEXING_INDICATOR_STANDALONE_TEXT_BOTTOM_GAP +
          indexingIndicatorCenteredOffset(textHeight, CURRENT_INDEXING_STANDALONE_PLUS_SIZE)};
}

inline constexpr int futureIndexingPageCountRightReservation(const int plusTextWidth) {
  return INDEXING_INDICATOR_AFTER_PAGE_COUNT_GAP +
         (plusTextWidth > FUTURE_INDEXING_DOT_SIZE ? plusTextWidth : FUTURE_INDEXING_DOT_SIZE);
}

inline constexpr int statusPageAndPercentTextWidth(const int pageTextWidth, const int percentTextWidth,
                                                   const int pagePercentGapWidth, const int plusTextWidth,
                                                   const bool futureIndexingActive) {
  return pageTextWidth + (futureIndexingActive ? futureIndexingPageCountRightReservation(plusTextWidth) : 0) +
         pagePercentGapWidth + percentTextWidth;
}

inline constexpr FutureIndexingDotPosition futureIndexingDotAfterPageCount(const int pageTextX, const int pageTextWidth,
                                                                           const int plusTextWidth, const int textY,
                                                                           const int textHeight) {
  (void)plusTextWidth;
  return FutureIndexingDotPosition{
      pageTextX + pageTextWidth + INDEXING_INDICATOR_AFTER_PAGE_COUNT_GAP,
      textY + indexingIndicatorCenteredOffset(textHeight, FUTURE_INDEXING_DOT_SIZE) + FUTURE_INDEXING_DOT_TEXT_Y_NUDGE};
}

inline constexpr FutureIndexingDotPosition futureIndexingDotBottomRight(
    const int screenWidth, const int screenHeight, const int orientedMarginRight, const int orientedMarginBottom,
    const int paddingBottom, const int reservedBottomHeight, const int statusBarVerticalMargin, const int textHeight) {
  return FutureIndexingDotPosition{
      screenWidth - orientedMarginRight - INDEXING_INDICATOR_STANDALONE_RIGHT_INSET - FUTURE_INDEXING_DOT_SIZE,
      screenHeight - orientedMarginBottom - paddingBottom - reservedBottomHeight - statusBarVerticalMargin -
          INDEXING_INDICATOR_STANDALONE_TEXT_BOTTOM_GAP +
          indexingIndicatorCenteredOffset(textHeight, FUTURE_INDEXING_DOT_SIZE) + FUTURE_INDEXING_DOT_TEXT_Y_NUDGE};
}

inline void formatStatusPageText(char* buffer, const size_t bufferSize, const StatusPageInfo& pageInfo) {
  if (buffer == nullptr || bufferSize == 0) {
    return;
  }

  if (pageInfo.totalPages == 0) {
    std::snprintf(buffer, bufferSize, "0/0");
    return;
  }

  const auto displayPage = static_cast<unsigned long>(pageInfo.currentPage + 1);
  if (pageInfo.totalKnown) {
    std::snprintf(buffer, bufferSize, "%lu/%lu", displayPage, static_cast<unsigned long>(pageInfo.totalPages));
    return;
  }

  if (pageInfo.hasMorePages) {
    std::snprintf(buffer, bufferSize, "%lu/%lu+", displayPage, static_cast<unsigned long>(pageInfo.totalPages));
    return;
  }

  std::snprintf(buffer, bufferSize, "%lu/?", displayPage);
}
