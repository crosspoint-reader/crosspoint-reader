// The CSS cascade as resolveStyle applies it: which of several matching rules wins.
//
// Every case is resolved through each store the resolver can serve from -- the rules of an in-RAM
// parse, the compiled cache through the heap index, and the two arena layouts a borrowed build
// uses (RESIDENT, INDEX-only) -- because each keeps rules in its own structure and all of them
// must agree.
#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../../lib/Epub/Epub/css/CssParser.h"
#include "BuildArena.h"
#include "HalStorage.h"

namespace {

std::string uniquePath(const char* suffix) {
  static std::atomic<unsigned> counter{0};
  return (std::filesystem::temp_directory_path() /
          ("csscascade-" + std::to_string(getpid()) + "-" + std::to_string(counter.fetch_add(1)) + suffix))
      .string();
}

std::string writeCss(const std::string& css) {
  const std::string path = uniquePath(".css");
  std::ofstream(path, std::ios::binary) << css;
  return path;
}

struct Probe {
  const char* tag;
  const char* classes;
  const char* id = "";
};

enum class Store { Parsed, HeapIndex, ArenaResident, ArenaIndex };
const char* storeName(const Store s) {
  switch (s) {
    case Store::Parsed:
      return "parsed";
    case Store::HeapIndex:
      return "heap index";
    case Store::ArenaResident:
      return "arena resident";
    case Store::ArenaIndex:
      return "arena index-only";
  }
  return "?";
}

// Parses the sheets (in order, as one book's stylesheets) and resolves `probe` from every store.
// `check` is called once per store with the resolved style.
template <typename Check>
void resolveEverywhere(const std::vector<std::string>& sheets, const Probe& probe, Check&& check) {
  std::vector<std::string> paths;
  for (const auto& css : sheets) paths.push_back(writeCss(css));
  const std::string cacheDir = uniquePath("");
  std::filesystem::create_directories(cacheDir);

  {
    CssParser parsed("");
    for (const auto& path : paths) {
      FsFile file;
      ASSERT_TRUE(Storage.openFileForRead("CSS", path.c_str(), file));
      ASSERT_TRUE(parsed.loadFromStream(file));
    }
    check(Store::Parsed, parsed.resolveStyle(probe.tag, probe.classes, probe.id));
  }

  CssParser parser(cacheDir);
  ASSERT_TRUE(parser.beginCacheCompile());
  for (const auto& path : paths) {
    FsFile file;
    ASSERT_TRUE(Storage.openFileForRead("CSS", path.c_str(), file));
    ASSERT_TRUE(parser.appendCompiledFromStream(file));
  }
  ASSERT_TRUE(parser.endCacheCompile());

  parser.clear();
  ASSERT_TRUE(parser.loadFromCache());
  check(Store::HeapIndex, parser.resolveStyle(probe.tag, probe.classes, probe.id));

  {
    BuildArena arena(64 * 1024);
    parser.clear();
    parser.setIndexArena(&arena);
    parser.setLeanResolve(true);
    ASSERT_TRUE(parser.loadFromCache());
    ASSERT_TRUE(parser.isArenaResident());
    check(Store::ArenaResident, parser.resolveStyle(probe.tag, probe.classes, probe.id));
    parser.clear();
  }

  // The smallest arena that still loads holds the index and nothing else: INDEX-only.
  for (size_t capacity = 8;; capacity += 4) {
    ASSERT_LT(capacity, 64u * 1024u) << "no arena size gave an INDEX-only load";
    BuildArena arena(capacity);
    parser.clear();
    parser.setIndexArena(&arena);
    parser.setLeanResolve(true);
    if (!parser.loadFromCache()) continue;
    ASSERT_FALSE(parser.isArenaResident());
    check(Store::ArenaIndex, parser.resolveStyle(probe.tag, probe.classes, probe.id));
    parser.clear();
    break;
  }

  std::error_code ec;
  std::filesystem::remove_all(cacheDir, ec);
  for (const auto& path : paths) std::filesystem::remove(path, ec);
}

void expectWeight(const std::vector<std::string>& sheets, const Probe& probe, const CssFontWeight expected) {
  resolveEverywhere(sheets, probe, [&](const Store store, const CssStyle& style) {
    EXPECT_TRUE(style.hasFontWeight()) << storeName(store);
    EXPECT_EQ(style.fontWeight, expected)
        << storeName(store) << ": <" << probe.tag << " class=\"" << probe.classes << "\">";
  });
}

void expectAlign(const std::vector<std::string>& sheets, const Probe& probe, const CssTextAlign expected) {
  resolveEverywhere(sheets, probe, [&](const Store store, const CssStyle& style) {
    EXPECT_TRUE(style.hasTextAlign()) << storeName(store);
    EXPECT_EQ(style.textAlign, expected) << storeName(store) << ": <" << probe.tag << " class=\"" << probe.classes
                                         << "\">";
  });
}

}  // namespace

// Two classes of equal specificity: the later rule in the stylesheet wins, whichever order the
// element lists them in (crosspoint-reader PR #3891 QA: `.z{bold} .a{normal}`, class="a z").
TEST(CssCascade, ClassOrderInTheAttributeDoesNotDecideTies) {
  const std::vector<std::string> css = {".z { font-weight: bold; }\n.a { font-weight: normal; }\n"};
  expectWeight(css, {"p", "z a"}, CssFontWeight::Normal);
  expectWeight(css, {"p", "a z"}, CssFontWeight::Normal);
}

// p.b (one class and one element) is more specific than .a (one class), so it wins even though .a
// comes later and the element lists b first.
TEST(CssCascade, ElementClassBeatsAPlainClassWhateverTheOrder) {
  const std::vector<std::string> css = {"p.b { text-align: right; }\n.a { text-align: center; }\n"};
  expectAlign(css, {"p", "b a"}, CssTextAlign::Right);
  expectAlign(css, {"p", "a b"}, CssTextAlign::Right);
}

// A rule naming the element loses to a class even when it comes later.
TEST(CssCascade, ClassBeatsAnElementRuleThatComesLater) {
  const std::vector<std::string> css = {".a { font-weight: bold; }\np { font-weight: normal; }\n"};
  expectWeight(css, {"p", "a"}, CssFontWeight::Bold);
}

// An id beats any number of classes and elements, wherever it stands.
TEST(CssCascade, IdBeatsClassesAndElements) {
  const std::vector<std::string> css = {
      "#x { text-align: left; }\np.a { text-align: center; }\n.b { text-align: right; }\n"};
  expectAlign(css, {"p", "a b", "x"}, CssTextAlign::Left);
}

// A selector declared again after another rule keeps its first declarations at their first place:
// `.a{bold} .b{normal} .a{italic}` leaves an element of both classes normal (the first .a lost to
// .b) and italic (the second .a). Merging the two .a rules used to make it bold.
TEST(CssCascade, RedeclaredSelectorKeepsItsEarlierPlace) {
  const std::vector<std::string> css = {
      ".a { font-weight: bold; }\n.b { font-weight: normal; }\n.a { font-style: italic; }\n"};
  for (const char* classes : {"a b", "b a"}) {
    resolveEverywhere(css, {"p", classes}, [&](const Store store, const CssStyle& style) {
      EXPECT_EQ(style.fontWeight, CssFontWeight::Normal) << storeName(store) << " class=\"" << classes << "\"";
      EXPECT_EQ(style.fontStyle, CssFontStyle::Italic) << storeName(store) << " class=\"" << classes << "\"";
    });
  }
}

// Redeclarations with nothing in between still merge into one rule, so the common
// `h1{...} h1{...}` costs no extra index entry.
TEST(CssCascade, AdjacentRedeclarationsMergeIntoOneRule) {
  const std::string path =
      writeCss(".a { font-weight: bold; }\n.a { font-style: italic; }\n.a, .b { margin-top: 1px; }\n");
  const std::string cacheDir = uniquePath("");
  std::filesystem::create_directories(cacheDir);
  CssParser parser(cacheDir);
  ASSERT_TRUE(parser.beginCacheCompile());
  {
    FsFile file;
    ASSERT_TRUE(Storage.openFileForRead("CSS", path.c_str(), file));
    ASSERT_TRUE(parser.appendCompiledFromStream(file));
  }
  ASSERT_TRUE(parser.endCacheCompile());
  EXPECT_EQ(parser.ruleCount(), 2u);  // .a (merged three times) and .b
  const CssStyle a = parser.resolveStyle("p", "a");
  EXPECT_EQ(a.fontWeight, CssFontWeight::Bold);
  EXPECT_EQ(a.fontStyle, CssFontStyle::Italic);
  EXPECT_TRUE(a.hasMarginTop());
  std::error_code ec;
  std::filesystem::remove_all(cacheDir, ec);
  std::filesystem::remove(path, ec);
}

// Stylesheet order runs across the book's stylesheets: a later sheet's rule wins a tie.
TEST(CssCascade, LaterStylesheetWinsTies) {
  const std::vector<std::string> css = {".b { font-weight: normal; }\n", ".a { font-weight: bold; }\n"};
  expectWeight(css, {"p", "a b"}, CssFontWeight::Bold);
  expectWeight(css, {"p", "b a"}, CssFontWeight::Bold);
}

// The background picture follows the same cascade (CssParser::backgroundImageFor).
TEST(CssCascade, BackgroundPictureFollowsTheCascade) {
  const std::string path = writeCss(
      "p.b { background: url(b.png) no-repeat; }\n.a { background: url(a.png) no-repeat; }\n"
      ".z { background: url(z.png) no-repeat; }\n.y { background: url(y.png) no-repeat; }\n");
  const std::string cacheDir = uniquePath("");
  std::filesystem::create_directories(cacheDir);
  {
    CssParser compiler(cacheDir);
    ASSERT_TRUE(compiler.beginCacheCompile());
    FsFile file;
    ASSERT_TRUE(Storage.openFileForRead("CSS", path.c_str(), file));
    ASSERT_TRUE(compiler.appendCompiledFromStream(file));
    ASSERT_TRUE(compiler.endCacheCompile());
  }
  CssParser parser(cacheDir);  // a later open: the pictures come back from the side file
  ASSERT_TRUE(parser.loadFromCache());
  const std::string* picture = parser.backgroundImageFor("p", "b a");
  ASSERT_NE(picture, nullptr);
  EXPECT_EQ(*picture, "b.png") << "p.b is more specific than .a";
  picture = parser.backgroundImageFor("div", "y z");
  ASSERT_NE(picture, nullptr);
  EXPECT_EQ(*picture, "y.png") << ".y comes after .z";
  std::error_code ec;
  std::filesystem::remove_all(cacheDir, ec);
  std::filesystem::remove(path, ec);
}
