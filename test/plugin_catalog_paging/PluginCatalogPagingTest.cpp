#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "PluginCatalogPaging.h"

using plugincatalogpaging::countsTowardPage;
using plugincatalogpaging::limitFor;
using plugincatalogpaging::PageWindow;
using plugincatalogpaging::Paging;
using plugincatalogpaging::pagingOf;
using Headers = std::vector<std::pair<std::string, std::string>>;

TEST(PluginCatalogPaging, ClassifiesTemplates) {
  const Headers none;
  EXPECT_EQ(pagingOf("https://x/all.json", "", none), Paging::Static);
  EXPECT_EQ(pagingOf("https://x/all-{page}.json", "", none), Paging::PageNumber);
  EXPECT_EQ(pagingOf("https://x/e?perPage={limit}&page={page}", "", none), Paging::SizedPages);
  EXPECT_EQ(pagingOf("https://x/s", "{\"page\":{page},\"per_page\":{limit}}", none), Paging::SizedPages);
  EXPECT_EQ(pagingOf("https://x/b?offset={offset}&limit={limit}", "", none), Paging::Offset);
  const Headers paged = {{"X-Page", "{page}"}};
  EXPECT_EQ(pagingOf("https://x/b", "", paged), Paging::PageNumber);
}

TEST(PluginCatalogPaging, LimitIsThePageSizeOnlyForSizedPages) {
  EXPECT_EQ(limitFor(Paging::SizedPages, 8), 8);
  EXPECT_EQ(limitFor(Paging::Offset, 8), 9);
  EXPECT_EQ(limitFor(Paging::PageNumber, 8), 9);
  EXPECT_EQ(limitFor(Paging::Static, 8), 9);
}

// Rows the server returned for one page, counted through a server window.
bool nextPageAfter(const Paging paging, const int rows, const int pageSize = 8) {
  PageWindow window(0, pageSize);
  for (int i = 0; i < rows; i++) window.take();
  return window.hasMore(paging);
}

TEST(PluginCatalogPaging, SizedPagesContinueAfterAFullPage) {
  EXPECT_TRUE(nextPageAfter(Paging::SizedPages, 8));
  EXPECT_FALSE(nextPageAfter(Paging::SizedPages, 7));
  EXPECT_FALSE(nextPageAfter(Paging::SizedPages, 0));
}

TEST(PluginCatalogPaging, OtherStylesNeedTheLookaheadRow) {
  for (const Paging paging : {Paging::Offset, Paging::PageNumber}) {
    EXPECT_FALSE(nextPageAfter(paging, 8));
    EXPECT_TRUE(nextPageAfter(paging, 9));
  }
}

TEST(PluginCatalogPaging, StaticFeedsSliceLocally) {
  PageWindow second(8, 8);  // page 2 of a 20-row file
  int shown = 0;
  for (int i = 0; i < 20; i++) shown += second.take() ? 1 : 0;
  EXPECT_EQ(shown, 8);
  EXPECT_TRUE(second.hasMore(Paging::Static));

  PageWindow last(16, 8);  // page 3: 4 rows
  shown = 0;
  for (int i = 0; i < 20; i++) shown += last.take() ? 1 : 0;
  EXPECT_EQ(shown, 4);
  EXPECT_FALSE(last.hasMore(Paging::Static));
}

// Rows the server returned for one page, `hidden` of them untitled (not shown).
bool nextPageWithHidden(const Paging paging, const int rows, const int hidden, const int pageSize = 8) {
  PageWindow window(0, pageSize);
  for (int i = 0; i < rows; i++) window.take(countsTowardPage(paging, i >= hidden));
  return window.hasMore(paging);
}

TEST(PluginCatalogPaging, HiddenRowsCountOnServerPages) {
  EXPECT_TRUE(nextPageWithHidden(Paging::Offset, 9, 1));
  EXPECT_TRUE(nextPageWithHidden(Paging::PageNumber, 9, 1));
  EXPECT_TRUE(nextPageWithHidden(Paging::SizedPages, 8, 1));
  EXPECT_TRUE(nextPageWithHidden(Paging::Offset, 9, 9));  // an all-untitled page still has a next one
  EXPECT_FALSE(nextPageWithHidden(Paging::Offset, 8, 1));
}

TEST(PluginCatalogPaging, StaticListsSkipHiddenRows) {
  PageWindow first(0, 8);  // 9 rows, 1 untitled: 8 shown, nothing left over
  int shown = 0;
  for (int i = 0; i < 9; i++) shown += first.take(countsTowardPage(Paging::Static, i != 3)) ? 1 : 0;
  EXPECT_EQ(shown, 8);
  EXPECT_FALSE(first.hasMore(Paging::Static));
}
