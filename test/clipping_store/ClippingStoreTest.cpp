#include <ClippingStore.h>
#include <HalStorage.h>
#include <gtest/gtest.h>

namespace {
using Result = ClippingStore::AddResult;
Result add(ClippingStore& store, const std::string& text = "one", const char* title = "Chapter") {
  return store.addClipping(0, 0, 0, 1, 0, 0, 1, title, 0, text, 1, 0, 3);
}
std::string storePath() {
  for (const auto& [path, node] : fake::files)
    if (path.ends_with(".bin")) return path;
  return "";
}
}  // namespace

TEST(ClippingStore, FailedLoadCannotPublishOrOverwriteValidPrefix) {
  fake::reset();
  ClippingStore store;
  ASSERT_TRUE(store.loadForBook("/book.epub", "Book", "Author", "epub"));
  ASSERT_EQ(add(store), Result::Added);
  ASSERT_EQ(add(store, "two"), Result::Added);
  store.unload();
  auto& bytes = fake::files.at(storePath())->bytes;
  bytes.pop_back();
  const auto damaged = bytes;
  EXPECT_FALSE(store.loadForBook("/book.epub", "Book", "Author", "epub"));
  EXPECT_EQ(store.clippingCount(), 0u);
  EXPECT_EQ(add(store), Result::SaveFailed);
  EXPECT_FALSE(store.saveToFile());
  EXPECT_FALSE(store.prepareSync());
  store.unload();
  EXPECT_EQ(bytes, damaged);
  ASSERT_TRUE(store.loadForBook("/other.epub", "Other", "", "epub"));
  ASSERT_EQ(add(store), Result::Added);
  EXPECT_FALSE(store.loadForBook("/bad.txt", "", "", "unsupported"));
  EXPECT_EQ(store.clippingCount(), 0u);
  EXPECT_EQ(add(store), Result::SaveFailed);
}

TEST(ClippingStore, AllocationFailureKeepsOldIndexAndUnloadReleasesIt) {
  fake::reset();
  ClippingStore store;
  ASSERT_TRUE(store.loadForBook("/book.epub", "Book", "Author", "epub"));
  for (int i = 0; i < 16; ++i) ASSERT_EQ(add(store), Result::Added);
  fake::failAlloc = 0;
  EXPECT_EQ(add(store), Result::SaveFailed);
  EXPECT_EQ(store.clippingCount(), 16u);
  std::string text;
  EXPECT_TRUE(store.readClippingText(3, text));
  EXPECT_EQ(text, "one");
  store.unload();
  EXPECT_EQ(store.clippingAt(0), nullptr);
  EXPECT_TRUE(store.loadForBook("/book.epub", "Book", "Author", "epub"));
  EXPECT_EQ(store.clippingCount(), 16u);
}

TEST(ClippingStore, CountLimitAndHeaderLimitsRoundTrip) {
  fake::reset();
  ClippingStore store;
  ASSERT_TRUE(store.loadForBook("/book.epub", "Book", "Author", "epub"));
  for (unsigned i = 0; i < CLIPPING_MAX_PER_BOOK - 1; ++i) ASSERT_EQ(add(store), Result::Added);
  store.unload();
  ASSERT_TRUE(store.loadForBook("/book.epub", "Book", "Author", "epub"));
  EXPECT_EQ(add(store), Result::Added);
  EXPECT_EQ(add(store), Result::LimitReached);
  store.unload();
  ASSERT_TRUE(store.loadForBook("/book.epub", "Book", "Author", "epub"));
  EXPECT_EQ(store.clippingCount(), CLIPPING_MAX_PER_BOOK);
  const auto original = fake::files.at(storePath())->bytes;
  store.unload();
  ASSERT_TRUE(store.loadForBook("/book.epub", std::string(4097, 'x'), "Author", "epub"));
  EXPECT_FALSE(store.removeClippingAt(0));
  store.unload();
  EXPECT_EQ(fake::files.at(storePath())->bytes, original);
}

TEST(ClippingStore, GrowthKeepsRecordsInPlaceAndDeletionRollsBackAcrossBlocks) {
  fake::reset();
  ClippingStore store;
  ASSERT_TRUE(store.loadForBook("/book.epub", "Book", "Author", "epub"));
  const Clipping* first = nullptr;
  for (unsigned i = 0; i < 33; ++i) {
    Clipping clipping;
    snprintf(clipping.id, sizeof(clipping.id), "clip-%u", i);
    const std::string text = std::to_string(i);
    clipping.textLength = text.size();
    ASSERT_TRUE(store.applyRemote(clipping, text, false));
    if (i == 0) first = store.clippingAt(0);
    EXPECT_EQ(store.clippingAt(0), first);
  }
  const auto path = storePath();
  const auto original = fake::files.at(path)->bytes;
  // The journal succeeds; the store rewrite fails after records have shifted.
  fake::failWrite = 1;
  EXPECT_FALSE(store.removeClippingAt(15));
  ASSERT_EQ(store.clippingCount(), 33u);
  EXPECT_EQ(fake::files.at(path)->bytes, original);
  for (unsigned i = 0; i < 33; ++i) {
    std::string text;
    ASSERT_TRUE(store.readClippingText(i, text));
    EXPECT_EQ(text, std::to_string(i));
  }
  const Clipping removed = *store.clippingAt(15);
  fake::failWrite = 0;
  EXPECT_FALSE(store.applyRemote(removed, "", true));
  EXPECT_EQ(store.clippingCount(), 33u);
  ASSERT_TRUE(store.applyRemote(removed, "", true));
  ASSERT_TRUE(store.removeClippingAt(15));
  store.unload();
  // Fail partway through allocating blocks while loading, then retry.
  fake::failAlloc = 1;
  EXPECT_FALSE(store.loadForBook("/book.epub", "Book", "Author", "epub"));
  EXPECT_EQ(store.clippingCount(), 0u);
  ASSERT_TRUE(store.loadForBook("/book.epub", "Book", "Author", "epub"));
  ASSERT_EQ(store.clippingCount(), 31u);
  for (unsigned i = 0; i < 31; ++i) {
    std::string text;
    ASSERT_TRUE(store.readClippingText(i, text));
    EXPECT_EQ(text, std::to_string(i < 15 ? i : i + 2));
  }
}

TEST(ClippingStore, OffsetAllocationFailureDoesNotCreateTempFile) {
  fake::reset();
  ClippingStore store;
  ASSERT_TRUE(store.loadForBook("/book.epub", "Book", "Author", "epub"));
  ASSERT_EQ(add(store), Result::Added);
  const auto path = storePath();
  const auto original = fake::files.at(path)->bytes;
  fake::failAlloc = 0;
  EXPECT_EQ(add(store, "two"), Result::SaveFailed);
  EXPECT_EQ(store.clippingCount(), 1u);
  EXPECT_EQ(fake::files.at(path)->bytes, original);
  EXPECT_FALSE(Storage.exists((path + ".tmp").c_str()));
  EXPECT_EQ(add(store, "two"), Result::Added);
}

TEST(ClippingStore, ShortDeletionAppendRollsBackAndRetryCanSync) {
  for (const bool failedRollback : {false, true}) {
    fake::reset();
    ClippingStore store;
    ASSERT_TRUE(store.loadForBook("/book.epub", "Book", "Author", "epub"));
    ASSERT_EQ(add(store), Result::Added);
    ASSERT_EQ(add(store, "two"), Result::Added);
    ASSERT_TRUE(store.prepareSync());
    const std::string firstId = store.clippingAt(0)->id;
    const std::string secondId = store.clippingAt(1)->id;
    ASSERT_TRUE(store.removeClippingAt(0));
    const auto journalPath = storePath() + ".deleted";
    const auto original = fake::files.at(journalPath)->bytes;
    fake::shortWrite = 1;
    if (failedRollback) fake::failTruncate = 0;
    EXPECT_FALSE(store.removeClippingAt(0));
    ASSERT_EQ(store.clippingCount(), 1u);
    if (failedRollback) {
      EXPECT_EQ(fake::files.at(journalPath)->bytes.size(), original.size() + 1);
    } else {
      EXPECT_EQ(fake::files.at(journalPath)->bytes, original);
    }
    ASSERT_TRUE(store.removeClippingAt(0));
    store.unload();
    ASSERT_TRUE(store.loadForBook("/book.epub", "Book", "Author", "epub"));
    uint32_t offset = 0;
    char id[65];
    ASSERT_TRUE(store.nextDeletion(offset, id));
    EXPECT_EQ(id, firstId);
    ASSERT_TRUE(store.nextDeletion(offset, id));
    EXPECT_EQ(id, secondId);
    ASSERT_TRUE(store.nextDeletion(offset, id));
    EXPECT_EQ(id[0], '\0');
    EXPECT_EQ(fake::files.at(journalPath)->bytes.size(), 130u);
    EXPECT_TRUE(store.finishDeletions());
  }
}

TEST(ClippingStore, InterruptedDeletionTailRecoversBeforeReading) {
  fake::reset();
  ClippingStore store;
  ASSERT_TRUE(store.loadForBook("/book.epub", "Book", "Author", "epub"));
  ASSERT_EQ(add(store), Result::Added);
  ASSERT_TRUE(store.prepareSync());
  const std::string expectedId = store.clippingAt(0)->id;
  ASSERT_TRUE(store.removeClippingAt(0));
  const auto journalPath = storePath() + ".deleted";
  const auto original = fake::files.at(journalPath)->bytes;
  fake::files.at(journalPath)->bytes.push_back('x');
  store.unload();
  ASSERT_TRUE(store.loadForBook("/book.epub", "Book", "Author", "epub"));
  uint32_t offset = 0;
  char id[65];
  fake::failTruncate = 0;
  EXPECT_FALSE(store.nextDeletion(offset, id));
  EXPECT_EQ(offset, 0u);
  ASSERT_TRUE(store.nextDeletion(offset, id));
  EXPECT_EQ(id, expectedId);
  EXPECT_EQ(fake::files.at(journalPath)->bytes, original);
  ASSERT_TRUE(store.nextDeletion(offset, id));
  EXPECT_EQ(id[0], '\0');
  ASSERT_TRUE(store.finishDeletions());
  fake::add(journalPath, "x");
  offset = 0;
  ASSERT_TRUE(store.nextDeletion(offset, id));
  EXPECT_EQ(id[0], '\0');
  EXPECT_TRUE(fake::files.at(journalPath)->bytes.empty());
}

TEST(ClippingStore, Utf8TitleAndFailedDeletionPreserveRecord) {
  fake::reset();
  ClippingStore store;
  ASSERT_TRUE(store.loadForBook("/book.epub", "Book", "Author", "epub"));
  ASSERT_EQ(add(store, "one", (std::string(46, 'x') + "가").c_str()), Result::Added);
  EXPECT_EQ(std::string(store.clippingAt(0)->chapterTitle), std::string(46, 'x'));
  fake::failWrite = 0;
  EXPECT_FALSE(store.removeClippingAt(0));
  ASSERT_EQ(store.clippingCount(), 1u);
  std::string text;
  EXPECT_TRUE(store.readClippingText(0, text));
  EXPECT_EQ(text, "one");
}

TEST(ClippingStore, MovesFolderWithClipsAndDeletionJournalAndRollsBackFailure) {
  for (const int failure : {-1, 0, 1, 2, 3}) {
    fake::reset();
    fake::add("/books/sub/book.epub");
    ClippingStore store;
    ASSERT_TRUE(store.loadForBook("/books/sub/book.epub", "Book", "Author", "epub"));
    ASSERT_EQ(add(store), Result::Added);
    ASSERT_EQ(add(store, "two"), Result::Added);
    ASSERT_TRUE(store.prepareSync());
    ASSERT_TRUE(store.removeClippingAt(0));
    store.unload();
    const auto oldStore = storePath();
    const auto original = fake::files.at(oldStore)->bytes;
    const auto deleted = fake::files.at(oldStore + ".deleted")->bytes;
    fake::failRename = failure;
    const bool moved = ClippingStore::moveBook("/books", "/renamed");
    if (moved) {
      EXPECT_FALSE(Storage.exists("/books/sub/book.epub"));
      EXPECT_TRUE(Storage.exists("/renamed/sub/book.epub"));
      EXPECT_FALSE(Storage.exists(oldStore.c_str()));
      ASSERT_TRUE(store.loadForBook("/renamed/sub/book.epub", "", "", "epub"));
      EXPECT_EQ(store.clippingCount(), 1u);
      std::string text;
      EXPECT_TRUE(store.readClippingText(0, text));
      EXPECT_EQ(text, "two");
      uint32_t offset = 0;
      char id[65];
      EXPECT_TRUE(store.nextDeletion(offset, id));
      EXPECT_NE(id[0], 0);
    } else {
      EXPECT_TRUE(Storage.exists("/books/sub/book.epub"));
      EXPECT_FALSE(Storage.exists("/renamed/sub/book.epub"));
      EXPECT_EQ(fake::files.at(oldStore)->bytes, original);
      EXPECT_EQ(fake::files.at(oldStore + ".deleted")->bytes, deleted);
    }
  }
}

TEST(ClippingStore, MoveRefusesExistingDestinationHistoryAndDeleteCleansSidecars) {
  fake::reset();
  fake::add("/one.epub");
  ClippingStore store;
  ASSERT_TRUE(store.loadForBook("/one.epub", "One", "", "epub"));
  ASSERT_EQ(add(store), Result::Added);
  store.unload();
  ASSERT_TRUE(store.loadForBook("/two.epub", "Two", "", "epub"));
  ASSERT_EQ(add(store, "two"), Result::Added);
  store.unload();
  EXPECT_FALSE(ClippingStore::moveBook("/one.epub", "/two.epub"));
  EXPECT_TRUE(Storage.exists("/one.epub"));
  EXPECT_TRUE(ClippingStore::deleteForFilePath("/two.epub", "epub"));
  EXPECT_TRUE(ClippingStore::moveBook("/one.epub", "/two.epub"));
  const auto path = storePath();
  fake::add(path + ".deleted", std::string(65, 'x'));
  fake::add(path + ".bak");
  fake::add(path + ".tmp");
  EXPECT_TRUE(ClippingStore::deleteForFilePath("/two.epub", "epub"));
  EXPECT_FALSE(Storage.exists(path.c_str()));
  EXPECT_FALSE(Storage.exists((path + ".deleted").c_str()));
  EXPECT_FALSE(Storage.exists((path + ".bak").c_str()));
  EXPECT_FALSE(Storage.exists((path + ".tmp").c_str()));
}
