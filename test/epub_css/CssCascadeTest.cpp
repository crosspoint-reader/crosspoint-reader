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

struct Element {
  const char* tag;
  const char* classes = "";
  const char* id = "";
};

struct Probe {
  const char* tag;
  const char* classes;
  const char* id = "";
  std::vector<Element> ancestors = {};  // outermost first; empty: resolved without an Ancestry
};

// Resolves the probe the way the chapter parser does: each ancestor's compound mask, then the
// element against them.
CssStyle resolve(const CssParser& parser, const Probe& probe, bool* dependsOnAncestors = nullptr) {
  if (probe.ancestors.empty())
    return parser.resolveStyle(probe.tag, probe.classes, probe.id, nullptr, dependsOnAncestors);
  std::vector<uint64_t> masks;
  for (const Element& e : probe.ancestors) masks.push_back(parser.compoundMask(e.tag, e.classes, e.id));
  CssParser::Ancestry ancestry;
  ancestry.masks = masks.data();
  ancestry.depth = masks.size();
  ancestry.self = parser.compoundMask(probe.tag, probe.classes, probe.id);
  return parser.resolveStyle(probe.tag, probe.classes, probe.id, &ancestry, dependsOnAncestors);
}

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
    check(Store::Parsed, resolve(parsed, probe));
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
  check(Store::HeapIndex, resolve(parser, probe));

  {
    BuildArena arena(64 * 1024);
    parser.clear();
    parser.setIndexArena(&arena);
    parser.setLeanResolve(true);
    ASSERT_TRUE(parser.loadFromCache());
    // A sheet of complex rules only has no indexed rule, so no RESIDENT index to report.
    if (parser.ruleCount() > 0) ASSERT_TRUE(parser.isArenaResident());
    check(Store::ArenaResident, resolve(parser, probe));
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
    check(Store::ArenaIndex, resolve(parser, probe));
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

// --- Compound selectors (.a.b, p.a.b, p#id.a, *) ---

// A compound matches only an element that has every one of its classes, in any order.
TEST(CssCascade, CompoundMatchesOnlyAnElementWithEveryClass) {
  const std::vector<std::string> css = {".a.b { font-weight: bold; }\n"};
  expectWeight(css, {"p", "a b"}, CssFontWeight::Bold);
  expectWeight(css, {"p", "b x a"}, CssFontWeight::Bold);
  for (const char* classes : {"a", "b", "a c", ""}) {
    resolveEverywhere(css, {"p", classes}, [&](const Store store, const CssStyle& style) {
      EXPECT_FALSE(style.hasFontWeight()) << storeName(store) << " class=\"" << classes << "\"";
    });
  }
}

// .a.b has two classes of specificity: it beats .a, .b and p.a even when they come later.
TEST(CssCascade, CompoundOutranksItsPartsWhereverItStands) {
  const std::vector<std::string> css = {
      ".a.b { text-align: right; }\n.a { text-align: center; }\n.b { text-align: left; }\n"
      "p.a { text-align: justify; }\n"};
  expectAlign(css, {"p", "a b"}, CssTextAlign::Right);
  expectAlign(css, {"p", "b a"}, CssTextAlign::Right);
}

// Two compounds of equal specificity: the later one wins.
TEST(CssCascade, LaterCompoundOfEqualSpecificityWins) {
  expectWeight({".a.b { font-weight: bold; }\n.c.d { font-weight: normal; }\n"}, {"p", "d c b a"},
               CssFontWeight::Normal);
  expectWeight({".c.d { font-weight: normal; }\n.a.b { font-weight: bold; }\n"}, {"p", "a b c d"}, CssFontWeight::Bold);
}

TEST(CssCascade, ElementCompoundNeedsTheElement) {
  const std::vector<std::string> css = {"p.a.b { margin-top: 3px; }\n"};
  resolveEverywhere(css, {"p", "b a"}, [&](const Store store, const CssStyle& style) {
    EXPECT_TRUE(style.hasMarginTop()) << storeName(store);
  });
  resolveEverywhere(css, {"div", "a b"}, [&](const Store store, const CssStyle& style) {
    EXPECT_FALSE(style.hasMarginTop()) << storeName(store);
  });
}

// p#x.a (one id, one class, one element) beats #x; without the class only #x applies.
TEST(CssCascade, IdAndClassCompound) {
  const std::vector<std::string> css = {"p#x.a { text-align: right; }\n#x { text-align: center; }\n"};
  expectAlign(css, {"p", "a", "x"}, CssTextAlign::Right);
  expectAlign(css, {"p", "", "x"}, CssTextAlign::Center);
  expectAlign(css, {"div", "a", "x"}, CssTextAlign::Center);
  expectAlign(css, {"p", "a", "X"}, CssTextAlign::Right);  // ids match case-insensitively, as #id rules do
}

// `*` matches every element at the lowest specificity; `*.a` is `.a`.
TEST(CssCascade, UniversalSelector) {
  const std::vector<std::string> css = {
      "p { font-style: normal; }\n* { font-style: italic; }\n*.note { font-weight: bold; }\n"};
  resolveEverywhere(css, {"p", ""}, [&](const Store store, const CssStyle& style) {
    EXPECT_EQ(style.fontStyle, CssFontStyle::Normal) << storeName(store) << ": p outranks *";
  });
  resolveEverywhere(css, {"span", "note"}, [&](const Store store, const CssStyle& style) {
    EXPECT_EQ(style.fontStyle, CssFontStyle::Italic) << storeName(store);
    EXPECT_EQ(style.fontWeight, CssFontWeight::Bold) << storeName(store) << ": *.note is .note";
  });
}

// Compounds are no longer counted unsupported; what the resolver still cannot use is.
TEST(CssCascade, CompoundsAreNoLongerUnsupported) {
  const std::string path =
      writeCss(".a.b, p#x.y, * { margin-top: 1px; }\na:hover, ul + p, [title] { margin-top: 2px; }\n");
  CssParser parser("");
  FsFile file;
  ASSERT_TRUE(Storage.openFileForRead("CSS", path.c_str(), file));
  ASSERT_TRUE(parser.loadFromStream(file));
  EXPECT_EQ(parser.selectorCandidates(), 6u);
  EXPECT_EQ(parser.unsupportedSelectors(), 3u);
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

// A more specific or later `avoid`/`auto` cancels a page break an earlier or broader rule forces.
TEST(CssCascade, AvoidCancelsAForcedPageBreak) {
  const std::vector<std::string> css = {
      ".chapter { page-break-before: always; }\n.chapter.first { page-break-before: avoid; }\n"
      "h2 { break-after: page; }\nh2 { break-after: auto; }\n"};
  resolveEverywhere(css, {"div", "chapter"}, [&](const Store store, const CssStyle& style) {
    EXPECT_TRUE(style.hasPageBreakBefore() && style.pageBreakBefore) << storeName(store);
  });
  resolveEverywhere(css, {"div", "first chapter"}, [&](const Store store, const CssStyle& style) {
    EXPECT_TRUE(style.hasPageBreakBefore()) << storeName(store);
    EXPECT_FALSE(style.pageBreakBefore) << storeName(store) << ": .chapter.first outranks .chapter";
  });
  resolveEverywhere(css, {"h2", ""}, [&](const Store store, const CssStyle& style) {
    EXPECT_TRUE(style.hasPageBreakAfter()) << storeName(store);
    EXPECT_FALSE(style.pageBreakAfter) << storeName(store) << ": the later h2 rule wins";
  });
}

// --- Descendant and child selectors ---

namespace {
void expectIndent(const std::vector<std::string>& sheets, const Probe& probe, const bool expected) {
  resolveEverywhere(sheets, probe, [&](const Store store, const CssStyle& style) {
    EXPECT_EQ(style.hasMarginLeft(), expected) << storeName(store) << ": <" << probe.tag << " class=\"" << probe.classes
                                               << "\"> under " << probe.ancestors.size() << " ancestors";
  });
}
}  // namespace

TEST(CssCascade, DescendantMatchesAnyAncestor) {
  const std::vector<std::string> css = {"blockquote p { margin-left: 2em; }\n"};
  expectIndent(css, {"p", "", "", {{"body"}, {"blockquote"}}}, true);
  expectIndent(css, {"p", "", "", {{"blockquote"}, {"div"}, {"section"}}}, true);
  expectIndent(css, {"p", "", "", {{"body"}, {"div"}}}, false);
  expectIndent(css, {"div", "", "", {{"blockquote"}}}, false);
  expectIndent(css, {"p", "", "", {}}, false);  // no ancestry: the rule cannot be checked, and is not applied
}

TEST(CssCascade, ChildNeedsTheParent) {
  const std::vector<std::string> css = {"div.poem > p { margin-left: 2em; }\n"};
  expectIndent(css, {"p", "", "", {{"body"}, {"div", "poem"}}}, true);
  expectIndent(css, {"p", "", "", {{"div", "poem"}, {"span"}}}, false);
  expectIndent(css, {"p", "", "", {{"body"}, {"div", "verse"}}}, false);
}

// `div.a > p span`: the span anywhere under a p whose parent is div.a.
TEST(CssCascade, MixedChainOfChildAndDescendant) {
  const std::vector<std::string> css = {"div.a > p span { margin-left: 1em; }\n"};
  expectIndent(css, {"span", "", "", {{"div", "a"}, {"p"}}}, true);
  expectIndent(css, {"span", "", "", {{"div", "a"}, {"p"}, {"em"}}}, true);
  expectIndent(css, {"span", "", "", {{"div", "a"}, {"section"}, {"p"}}}, false);
}

// The nearest ancestor that fits a step is not always the one that completes the chain:
// `a > b c` under a, b, x, b (outermost first) matches through the outer b, not the inner one.
TEST(CssCascade, ChildRunIsRetriedFurtherUp) {
  const std::vector<std::string> css = {"a > b c { margin-left: 1em; }\n"};
  expectIndent(css, {"c", "", "", {{"a"}, {"b"}, {"x"}, {"b"}}}, true);
  expectIndent(css, {"c", "", "", {{"y"}, {"b"}, {"x"}, {"b"}}}, false);
}

// Deep nesting with a chain that cannot match: the answer is still no, after a linear scan.
TEST(CssCascade, DeepNestingWithAFailingChain) {
  const std::vector<std::string> css = {
      "x div div div div div div p { margin-left: 1em; }\ndiv div div div div div div p { margin-top: 1px; }\n"};
  std::vector<Element> divs(32, Element{"div"});
  resolveEverywhere(css, {"p", "", "", divs}, [&](const Store store, const CssStyle& style) {
    EXPECT_FALSE(style.hasMarginLeft()) << storeName(store);
    EXPECT_TRUE(style.hasMarginTop()) << storeName(store);
  });
}

TEST(CssCascade, UniversalChildAndIdAncestor) {
  const std::vector<std::string> css = {"#toc > * { margin-left: 1em; }\n"};
  expectIndent(css, {"li", "", "", {{"body"}, {"ul", "", "toc"}}}, true);
  expectIndent(css, {"li", "", "", {{"ul", "", "toc"}, {"li"}}}, false);
}

// A contextual rule ranks by its whole selector: `blockquote p` (two element names) beats a later
// `p`; `.note p` and `p.x` tie at one class and one element, so the later one wins.
TEST(CssCascade, ContextualSpecificityAndOrder) {
  expectAlign({"blockquote p { text-align: right; }\np { text-align: center; }\n"}, {"p", "", "", {{"blockquote"}}},
              CssTextAlign::Right);
  expectAlign({".note p { text-align: right; }\np.x { text-align: center; }\n"}, {"p", "x", "", {{"div", "note"}}},
              CssTextAlign::Center);
  expectAlign({"p.x { text-align: center; }\n.note p { text-align: right; }\n"}, {"p", "x", "", {{"div", "note"}}},
              CssTextAlign::Right);
}

// What the chapter parser's memo relies on: a style that a descendant or child rule could reach
// is flagged, matched or not, and nothing else is.
TEST(CssCascade, ContextDependenceIsReported) {
  const std::string path =
      writeCss("blockquote p { margin-left: 2em; }\n.a.b { margin-top: 1px; }\nspan { margin-top: 2px; }\n");
  CssParser parser("");
  FsFile file;
  ASSERT_TRUE(Storage.openFileForRead("CSS", path.c_str(), file));
  ASSERT_TRUE(parser.loadFromStream(file));
  ASSERT_TRUE(parser.hasComplexRules());
  bool depends = false;
  resolve(parser, {"p", "", "", {{"body"}}}, &depends);
  EXPECT_TRUE(depends) << "p outside a blockquote: the rule could have applied";
  resolve(parser, {"p", "", "", {{"blockquote"}}}, &depends);
  EXPECT_TRUE(depends);
  resolve(parser, {"span", "a b", "", {{"blockquote"}}}, &depends);
  EXPECT_FALSE(depends) << "no descendant or child rule ends in a span";
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

TEST(CssCascade, SiblingAndOverlongChainsStayUnsupported) {
  const std::string path = writeCss(
      "h1 + p, h1 ~ p, a b c d e f g h i, div >> p, > p { margin-top: 1px; }\nul li, ol > li { margin-top: 2px; }\n");
  CssParser parser("");
  FsFile file;
  ASSERT_TRUE(Storage.openFileForRead("CSS", path.c_str(), file));
  ASSERT_TRUE(parser.loadFromStream(file));
  EXPECT_EQ(parser.selectorCandidates(), 7u);
  EXPECT_EQ(parser.unsupportedSelectors(), 5u);
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

// --- inherit ---

// A more specific `inherit` beats the broader rule's value in every store -- the RESIDENT pool's
// compressed form included -- and reports the property unset, so the element takes its parent's.
TEST(CssCascade, InheritBeatsALowerRuleInEveryStore) {
  const std::vector<std::string> css = {
      "p { font-weight: bold; font-style: italic; text-align: center; }\n"
      "p.x { font-weight: inherit; font-style: inherit; text-align: inherit; }\n"
      "p.y { font-weight: inherit; }\np.y { font-weight: bold; }\n"};
  resolveEverywhere(css, {"p", "x"}, [&](const Store store, const CssStyle& style) {
    EXPECT_FALSE(style.hasFontWeight()) << storeName(store);
    EXPECT_FALSE(style.hasFontStyle()) << storeName(store);
    EXPECT_FALSE(style.hasTextAlign()) << storeName(store);
  });
  expectWeight(css, {"p", "y"}, CssFontWeight::Bold);  // a later bold beats the inherit
  resolveEverywhere(css, {"p", ""}, [&](const Store store, const CssStyle& style) {
    EXPECT_EQ(style.fontStyle, CssFontStyle::Italic) << storeName(store);
  });
}
