#include <FlatMap.h>
#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <unordered_map>

// Hasher that accepts string_view for lookup (mirrors CssParser usage)
struct SvHash {
  using is_transparent = void;
  size_t operator()(std::string_view sv) const { return std::hash<std::string_view>{}(sv); }
  size_t operator()(const std::string& s) const { return std::hash<std::string_view>{}(s); }
};

struct SvEqual {
  using is_transparent = void;
  bool operator()(const std::string& a, std::string_view b) const { return a == b; }
  bool operator()(std::string_view a, const std::string& b) const { return a == b; }
  bool operator()(const std::string& a, const std::string& b) const { return a == b; }
};

using CssMap = FlatMap<std::string, std::string, SvHash, SvEqual>;

struct IntHash {
  size_t operator()(int value) const { return static_cast<size_t>(value); }
};

// Basic insert and lookup
TEST(FlatMap, EmplaceAndFind) {
  CssMap m;
  m.emplace("color", "red");
  m.emplace("font-size", "14px");

  auto it = m.find(std::string_view("color"));
  ASSERT_NE(it, m.end());
  EXPECT_EQ(it->second, "red");

  it = m.find(std::string_view("font-size"));
  ASSERT_NE(it, m.end());
  EXPECT_EQ(it->second, "14px");
}

// Missing key returns end()
TEST(FlatMap, FindMissing) {
  CssMap m;
  m.emplace("color", "blue");
  EXPECT_EQ(m.find(std::string_view("background")), m.end());
}

// Duplicate key updates value
TEST(FlatMap, EmplaceDuplicateUpdates) {
  CssMap m;
  m.emplace("color", "red");
  m.emplace("color", "blue");
  EXPECT_EQ(m.size(), 1u);
  auto it = m.find(std::string_view("color"));
  ASSERT_NE(it, m.end());
  EXPECT_EQ(it->second, "blue");
}

// Trigger multiple rehashes via many inserts
TEST(FlatMap, ManyInsertsTriggerRehash) {
  CssMap m;
  const int N = 200;
  std::unordered_map<std::string, std::string> ref;

  for (int i = 0; i < N; i++) {
    std::string key = "prop-" + std::to_string(i);
    std::string val = "val-" + std::to_string(i);
    m.emplace(key, val);
    ref[key] = val;
  }

  EXPECT_EQ(m.size(), (size_t)N);

  for (auto& [k, v] : ref) {
    auto it = m.find(std::string_view(k));
    ASSERT_NE(it, m.end()) << "missing key: " << k;
    EXPECT_EQ(it->second, v);
  }
}

// Reserve then insert up to reserved capacity without rehash
TEST(FlatMap, ReserveAndFill) {
  CssMap m;
  m.reserve(64);

  for (int i = 0; i < 64; i++) {
    m.emplace("k" + std::to_string(i), "v" + std::to_string(i));
  }

  EXPECT_EQ(m.size(), 64u);

  for (int i = 0; i < 64; i++) {
    std::string key = "k" + std::to_string(i);
    auto it = m.find(std::string_view(key));
    ASSERT_NE(it, m.end()) << "missing: " << key;
    EXPECT_EQ(it->second, "v" + std::to_string(i));
  }
}

// Exact reserve can still end up full; Robin Hood probing should keep a miss
// on a displaced keyspace correct even without an empty-slot terminator.
TEST(FlatMap, FindMissingInFullTable) {
  FlatMap<int, int, IntHash> m;
  m.reserve(4);
  m.emplace(0, 10);
  m.emplace(4, 20);
  m.emplace(8, 30);
  m.emplace(12, 40);

  EXPECT_EQ(m.size(), 4u);
  EXPECT_EQ(m.find(16), m.end());
}

// After rehash, all previously inserted keys are still findable
TEST(FlatMap, RehashPreservesAllEntries) {
  CssMap m;
  m.reserve(4);  // Small initial capacity to force rehash

  std::vector<std::pair<std::string, std::string>> entries = {
      {"color", "red"},     {"background", "blue"},   {"margin", "0"}, {"padding", "4px"}, {"font-size", "12px"},
      {"display", "block"}, {"position", "relative"}, {"top", "0"},    {"left", "0"},      {"right", "0"},
  };

  for (auto& [k, v] : entries) {
    m.emplace(k, v);
  }

  EXPECT_EQ(m.size(), entries.size());

  for (auto& [k, v] : entries) {
    auto it = m.find(std::string_view(k));
    ASSERT_NE(it, m.end()) << "lost after rehash: " << k;
    EXPECT_EQ(it->second, v);
  }
}

// Iteration visits every entry exactly once
TEST(FlatMap, IterationIsComplete) {
  CssMap m;
  std::unordered_map<std::string, std::string> ref;
  for (int i = 0; i < 50; i++) {
    auto k = "k" + std::to_string(i);
    auto v = "v" + std::to_string(i);
    m.emplace(k, v);
    ref[k] = v;
  }

  std::unordered_map<std::string, std::string> seen;
  for (auto e : m) {
    EXPECT_FALSE(seen.count(e.first)) << "duplicate iteration: " << e.first;
    seen[e.first] = e.second;
  }

  EXPECT_EQ(seen, ref);
}

// clear() resets the map
TEST(FlatMap, Clear) {
  CssMap m;
  m.emplace("a", "1");
  m.emplace("b", "2");
  m.clear();
  EXPECT_EQ(m.size(), 0u);
  EXPECT_TRUE(m.empty());
  EXPECT_EQ(m.find(std::string_view("a")), m.end());

  // Re-insert after clear should work
  m.emplace("a", "3");
  auto it = m.find(std::string_view("a"));
  ASSERT_NE(it, m.end());
  EXPECT_EQ(it->second, "3");
}

// operator[] inserts default and returns reference
TEST(FlatMap, OperatorBracket) {
  FlatMap<std::string, int> m;
  m["x"] = 42;
  EXPECT_EQ(m.size(), 1u);
  EXPECT_EQ(m["x"], 42);
  m["x"] = 99;
  EXPECT_EQ(m.size(), 1u);
  EXPECT_EQ(m["x"], 99);
}

// Stress: insert the same key repeatedly, size stays 1
TEST(FlatMap, StressDuplicateKey) {
  CssMap m;
  for (int i = 0; i < 1000; i++) {
    m.emplace("only-key", std::to_string(i));
  }
  EXPECT_EQ(m.size(), 1u);
  auto it = m.find(std::string_view("only-key"));
  ASSERT_NE(it, m.end());
  EXPECT_EQ(it->second, "999");
}
