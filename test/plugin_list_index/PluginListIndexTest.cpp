#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "PluginListIndex.h"

using namespace pluginlistindex;

namespace {

struct List {
  std::string title, url;
};

using Fetched = std::vector<std::pair<std::string, std::string>>;  // title, url

// Same rule as the catalog: a list needs a title and a URL.
bool readList(const std::pair<std::string, std::string>& source, List& list) {
  list.title = source.first;
  list.url = source.second;
  return !list.title.empty() && !list.url.empty();
}

std::vector<std::string> titles(const std::vector<List>& lists) {
  std::vector<std::string> out;
  for (const auto& list : lists) out.push_back(list.title);
  return out;
}

}  // namespace

TEST(PluginListIndex, FetchedListsFollowFixedOnes) {
  std::vector<List> lists = {{"Home", "/home"}, {"Archive", "/archive"}};
  EXPECT_TRUE(replaceFetched(lists, 2, Fetched{{"Work", "/f/1"}, {"Later", "/f/2"}}, readList));
  EXPECT_EQ(titles(lists), (std::vector<std::string>{"Home", "Archive", "Work", "Later"}));
}

TEST(PluginListIndex, FetchedListsWithoutFixedOnes) {
  std::vector<List> lists;
  EXPECT_TRUE(replaceFetched(lists, 0, Fetched{{"Work", "/f/1"}}, readList));
  EXPECT_EQ(titles(lists), (std::vector<std::string>{"Work"}));
}

TEST(PluginListIndex, FetchingAgainReplacesEarlierFetchedLists) {
  std::vector<List> lists = {{"Home", "/home"}};
  ASSERT_TRUE(replaceFetched(lists, 1, Fetched{{"Work", "/f/1"}, {"Later", "/f/2"}}, readList));
  EXPECT_TRUE(replaceFetched(lists, 1, Fetched{{"Travel", "/f/3"}}, readList));
  EXPECT_EQ(titles(lists), (std::vector<std::string>{"Home", "Travel"}));
}

TEST(PluginListIndex, EntriesWithoutTitleOrUrlAreSkipped) {
  std::vector<List> lists = {{"Home", "/home"}};
  EXPECT_TRUE(replaceFetched(lists, 1, Fetched{{"", "/f/1"}, {"Work", ""}, {"Later", "/f/2"}}, readList));
  EXPECT_EQ(titles(lists), (std::vector<std::string>{"Home", "Later"}));
}

TEST(PluginListIndex, NoUsableEntryKeepsOnlyFixedLists) {
  std::vector<List> lists = {{"Home", "/home"}};
  EXPECT_FALSE(replaceFetched(lists, 1, Fetched{{"", "/f/1"}, {"Work", ""}}, readList));
  EXPECT_EQ(titles(lists), (std::vector<std::string>{"Home"}));
  EXPECT_FALSE(replaceFetched(lists, 1, Fetched{}, readList));
  EXPECT_EQ(titles(lists), (std::vector<std::string>{"Home"}));
}

TEST(PluginListIndex, OutcomeOfAFetch) {
  EXPECT_EQ(outcomeOf(true, 0), Outcome::Fetched);
  EXPECT_EQ(outcomeOf(true, 2), Outcome::Fetched);
  EXPECT_EQ(outcomeOf(false, 2), Outcome::FixedOnly);
  EXPECT_EQ(outcomeOf(false, 0), Outcome::Failed);
}
