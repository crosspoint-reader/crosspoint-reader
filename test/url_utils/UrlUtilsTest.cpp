#include <UrlUtils.h>
#include <gtest/gtest.h>
TEST(UrlUtils, ResolvesReferencesAgainstFeedFile) {
  const std::string base = "https://catalog.example/dir/catalog.atom?old=1#old";
  EXPECT_EQ(UrlUtils::buildUrl(base, "books/item.epub"), "https://catalog.example/dir/books/item.epub");
  EXPECT_EQ(UrlUtils::buildUrl(base, "../item.epub"), "https://catalog.example/item.epub");
  EXPECT_EQ(UrlUtils::buildUrl(base, "?page=2"), "https://catalog.example/dir/catalog.atom?page=2");
  EXPECT_EQ(UrlUtils::buildUrl(base, "#new"), "https://catalog.example/dir/catalog.atom?old=1#new");
  EXPECT_EQ(UrlUtils::buildUrl(base, "/book.epub"), "https://catalog.example/book.epub");
  EXPECT_EQ(UrlUtils::buildUrl(base, "//other.example/book.epub"), "https://other.example/book.epub");
  EXPECT_EQ(UrlUtils::buildUrl(base, "a b%20c.epub"), "https://catalog.example/dir/a%20b%20c.epub");
}
TEST(UrlUtils, AuthorityAndTrailingDirectoryBasesWork) {
  EXPECT_EQ(UrlUtils::buildUrl("catalog.example", "book.epub"), "http://catalog.example/book.epub");
  EXPECT_EQ(UrlUtils::buildUrl("https://catalog.example?old=1", "book.epub"), "https://catalog.example/book.epub");
  EXPECT_EQ(UrlUtils::buildUrl("https://catalog.example/dir/", "book.epub"), "https://catalog.example/dir/book.epub");
  EXPECT_EQ(UrlUtils::buildUrl("https://catalog.example/dir/feed", "../../book.epub"),
            "https://catalog.example/book.epub");
}
