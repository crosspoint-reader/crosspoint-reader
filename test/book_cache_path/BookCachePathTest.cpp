#include <gtest/gtest.h>

#include <string>

#include "BookCachePath.h"

namespace {

constexpr std::string_view ROOT = "/.crosspoint";

// The hash digits depend on the standard library, so the tests pin the shape of
// a cache path and the relations between paths, never a literal directory name.
std::string hashPart(const std::string& cachePath, std::string_view prefix) {
  const std::string head = std::string(ROOT) + "/" + std::string(prefix);
  EXPECT_EQ(cachePath.substr(0, head.size()), head);
  return cachePath.substr(head.size());
}

TEST(BookCachePath, PlacesBookUnderRootWithTypePrefix) {
  const std::string path = BookCachePath::forBook(ROOT, BookCachePath::EPUB_PREFIX, "/books/dune.epub");
  const std::string hash = hashPart(path, BookCachePath::EPUB_PREFIX);
  EXPECT_FALSE(hash.empty());
  EXPECT_EQ(hash.find_first_not_of("0123456789"), std::string::npos);
}

TEST(BookCachePath, SamePathAlwaysMapsToSameDirectory) {
  const std::string first = BookCachePath::forBook(ROOT, BookCachePath::EPUB_PREFIX, "/books/dune.epub");
  const std::string second = BookCachePath::forBook(ROOT, BookCachePath::EPUB_PREFIX, "/books/dune.epub");
  EXPECT_EQ(first, second);
}

TEST(BookCachePath, MovingABookChangesItsDirectory) {
  const std::string before = BookCachePath::forBook(ROOT, BookCachePath::EPUB_PREFIX, "/books/dune.epub");
  const std::string afterRename = BookCachePath::forBook(ROOT, BookCachePath::EPUB_PREFIX, "/books/Dune.epub");
  const std::string afterMove = BookCachePath::forBook(ROOT, BookCachePath::EPUB_PREFIX, "/read/dune.epub");
  EXPECT_NE(before, afterRename);
  EXPECT_NE(before, afterMove);
}

TEST(BookCachePath, TypesWithTheSamePathDoNotShareADirectory) {
  const std::string epub = BookCachePath::forBook(ROOT, BookCachePath::EPUB_PREFIX, "/books/dune");
  const std::string xtc = BookCachePath::forBook(ROOT, BookCachePath::XTC_PREFIX, "/books/dune");
  const std::string txt = BookCachePath::forBook(ROOT, BookCachePath::TXT_PREFIX, "/books/dune");
  EXPECT_NE(epub, xtc);
  EXPECT_NE(epub, txt);
  EXPECT_NE(xtc, txt);
}

TEST(BookCachePath, HashDependsOnPathOnlyNotOnRoot) {
  const std::string underRoot = BookCachePath::forBook(ROOT, BookCachePath::EPUB_PREFIX, "/books/dune.epub");
  const std::string underOther = BookCachePath::forBook("/other", BookCachePath::EPUB_PREFIX, "/books/dune.epub");
  EXPECT_EQ(underOther.substr(0, 7), "/other/");
  EXPECT_EQ(hashPart(underRoot, BookCachePath::EPUB_PREFIX), underOther.substr(std::string("/other/epub_").size()));
}

TEST(BookCachePath, RecognisesCacheDirectoryNames) {
  EXPECT_TRUE(BookCachePath::isCacheDirName("epub_123"));
  EXPECT_TRUE(BookCachePath::isCacheDirName("xtc_123"));
  EXPECT_TRUE(BookCachePath::isCacheDirName("txt_123"));
}

TEST(BookCachePath, RejectsNamesThatAreNotBookCaches) {
  EXPECT_FALSE(BookCachePath::isCacheDirName(""));
  EXPECT_FALSE(BookCachePath::isCacheDirName("epub"));
  EXPECT_FALSE(BookCachePath::isCacheDirName("fonts"));
  EXPECT_FALSE(BookCachePath::isCacheDirName("EPUB_123"));
  EXPECT_FALSE(BookCachePath::isCacheDirName("my_epub_123"));
}

// The name decides what "Clear Cache" deletes, so a directory that merely
// starts like one must not be swept up with the real caches.
TEST(BookCachePath, RejectsAPrefixWithoutAHashAfterIt) {
  EXPECT_FALSE(BookCachePath::isCacheDirName("epub_"));
  EXPECT_FALSE(BookCachePath::isCacheDirName("epub_notes"));
  EXPECT_FALSE(BookCachePath::isCacheDirName("epub_123abc"));
  EXPECT_FALSE(BookCachePath::isCacheDirName("txt_ 12"));
  EXPECT_TRUE(BookCachePath::isCacheDirName("epub_0"));
}

TEST(BookCachePath, GeneratedNameIsRecognisedAsACacheDirectory) {
  const std::string path = BookCachePath::forBook(ROOT, BookCachePath::TXT_PREFIX, "/books/notes.txt");
  const std::string name = path.substr(path.rfind('/') + 1);
  EXPECT_TRUE(BookCachePath::isCacheDirName(name));
}

}  // namespace
