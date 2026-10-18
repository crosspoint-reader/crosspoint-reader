// Full-pipeline equivalence tests over the synthetic corpus:
//  1. Warm-cache equivalence — rebuilding over the book-level caches a cold
//     run left behind dumps identically to that run.
//  2. Golden equivalence — two cold builds of every synthetic corpus book dump
//     identically to each other and to the committed golden. Regenerate
//     intentionally changed goldens with: UPDATE_GOLDENS=1 ctest -R EpubPipeline
//
// Every book runs with font-size normalization OFF (the tight ±3% float-rounding
// dead zone). The books whose layout normalization actually changes also run
// with it ON (the ±10% band that snaps publisher near-body
// <span font-size:0.92em> wrappers back to native size), against a golden of
// their own: <book>.golden.txt for OFF, <book>_norm.golden.txt for ON.
//
// The default goldens run with hyphenation and bionic reading off, so they reach one
// line breaker of four. The prose books therefore also run with hyphenation ON
// (<book>_hyph.golden.txt: the two greedy breakers and the page-bottom retry), and
// a few with bionic reading ON (<book>_bionic.golden.txt: the word-list rebuild).
//
// The books set in several font families also run with the families Publisher Fonts lays their blocks out in:
// Accents (<book>_accents.golden.txt) and All Text (<book>_alltext.golden.txt). Off and Body Text are the default
// golden (Body Text changes only the body font ID, which the shim renderer draws alike).
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "Epub/BlockFontFamilies.h"
#include "Epub/css/CssParser.h"
#include "PipelineRunner.h"

namespace fs = std::filesystem;

namespace {

std::string freshCacheDir(const std::string& tag) {
  const auto dir = fs::temp_directory_path() / "epub_pipeline_test" / tag;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir.string();
}

// The render setting a case turns ON; every other one stays as in the default.
enum class Variant { Default, Normalization, Hyphenation, Bionic, Accents, AllText };

// One corpus book under one variant.
struct Case {
  std::string epub;
  Variant variant = Variant::Default;
};

const char* variantName(const Variant v) {
  switch (v) {
    case Variant::Normalization:
      return "fontSizeNormalization";
    case Variant::Hyphenation:
      return "hyphenation";
    case Variant::Bionic:
      return "bionicReading";
    case Variant::Accents:
      return "publisherFontsAccents";
    case Variant::AllText:
      return "publisherFontsAllText";
    case Variant::Default:
      break;
  }
  return "default";
}

// Without this gtest dumps the struct as raw bytes in failure messages.
void PrintTo(const Case& c, std::ostream* os) {
  *os << fs::path(c.epub).filename().string() << " [" << variantName(c.variant) << "]";
}

std::string stem(const std::string& path) { return fs::path(path).stem().string(); }

// The corpus books whose layout normalization changes. For every other book the ON dump is
// byte-identical to OFF, so an ON case would only re-assert the OFF golden under a second name.
const char* const kNormalizationSensitiveBooks[] = {"test_font_normalization", "test_font_sizes"};

// Books with enough running text that hyphenation splits words, and a hyphenated line lands at
// the bottom of a page (the retry).
const char* const kHyphenationBooks[] = {"test_anchor_pagebreak", "test_kerning_ligature", "test_wrapped_paragraphs"};

// Bionic reading bolds a prefix of every word. Paragraphs over 96 words (test_long_paragraph_spacing
// has seven) are laid out before they end, and the rebuild resumes where it stopped.
const char* const kBionicBooks[] = {"test_long_paragraph_spacing", "test_text_rendering"};

// Books set in several font families (see test/epubs/make_test_font_families.py).
const char* const kFontFamilyBooks[] = {"test_font_families"};

// The families as the reader builds them for a 14 pt built-in body (buildBookFontFamilies), with stand-in IDs: the
// built-in sans-serif's rungs are 2000 + their size, the accent family's native sizes 0x12345600 | px. Accents also
// has the book's accent family; All Text has a TTF body, Book Body, and so no room for one.
BlockFontFamilies publisherFontFamilies(const bool accentFamily) {
  BlockFontFamilies families;
  families.body = CssParser::fontFamilyHash("Book Body") | CssFontFamily::kSerif;
  FontSizeLadder sans;
  for (const uint16_t pt : {10, 12, 14, 16, 18, 20, 22, 24, 26}) sans.addRung(2000 + pt, pt * 100 / 14);
  families.add(CssFontFamily::kSansSerif, 2014, sans);
  if (accentFamily) {
    FontSizeLadder native;
    native.setNative(0x12345600, (14 * 64 * 150 + 36) / 72, 10, 120);
    families.add(CssParser::fontFamilyHash("Book Display"), 0x777, native);
  }
  return families;
}
const BlockFontFamilies kAccentsFamilies = publisherFontFamilies(true);
const BlockFontFamilies kAllTextFamilies = publisherFontFamilies(false);

// Distinguishes the variants' cache dirs and golden files. The default keeps the
// historical unsuffixed golden name so its committed layout stays reviewable
// across the normalization change.
std::string variantSuffix(const Case& c) {
  switch (c.variant) {
    case Variant::Normalization:
      return "_norm";
    case Variant::Hyphenation:
      return "_hyph";
    case Variant::Bionic:
      return "_bionic";
    case Variant::Accents:
      return "_accents";
    case Variant::AllText:
      return "_alltext";
    case Variant::Default:
      break;
  }
  return "";
}

std::string runOnce(const Case& c, const std::string& cacheDir) {
  pipeline_harness::Profile profile;
  profile.fontSizeNormalization = c.variant == Variant::Normalization;
  profile.hyphenationEnabled = c.variant == Variant::Hyphenation;
  profile.bionicReadingEnabled = c.variant == Variant::Bionic;
  if (c.variant == Variant::Accents) profile.fontFamilies = &kAccentsFamilies;
  if (c.variant == Variant::AllText) profile.fontFamilies = &kAllTextFamilies;
  std::ostringstream dump;
  const bool ok = pipeline_harness::runAndDump(c.epub, cacheDir, profile, dump);
  EXPECT_TRUE(ok) << "pipeline failed for " << c.epub << " (" << variantName(c.variant) << ")\n" << dump.str();
  return dump.str();
}

// Unique per (book, variant) so the two variants never share a cache dir.
std::string caseCacheDir(const Case& c, const std::string& tag) {
  return freshCacheDir(stem(c.epub) + variantSuffix(c) + "_" + tag);
}

bool readFile(const fs::path& path, std::string& out) {
  std::ifstream in(path);
  if (!in) return false;
  std::stringstream bytes;
  bytes << in.rdbuf();
  out = bytes.str();
  return true;
}

class EpubPipelineTest : public testing::TestWithParam<Case> {};

// runAndDump always rebuilds every section, so the second run is not a page cache served without a
// build. What it does read back instead of the archive is the book-level state the first run left:
// book.bin, the compiled CSS index, the image manifest, the banked chapter XHTML and the footnote
// store with its resolved-spine bits.
TEST_P(EpubPipelineTest, RebuildOverWarmBookCacheMatchesColdRun) {
  const Case c = GetParam();
  const std::string cacheDir = caseCacheDir(c, "warm");
  const std::string cold = runOnce(c, cacheDir);
  const std::string warm = runOnce(c, cacheDir);
  EXPECT_EQ(cold, warm) << "rebuilding " << c.epub << " over its warm book cache changed the layout";
}

// Built twice, into separate cache dirs: a nondeterministic build (an uninitialised field, a
// pointer-ordered container) can match the golden once by luck, and must not be allowed to write one.
TEST_P(EpubPipelineTest, MatchesGolden) {
  const Case c = GetParam();
  const std::string dump = runOnce(c, caseCacheDir(c, "a"));
  ASSERT_EQ(dump, runOnce(c, caseCacheDir(c, "b"))) << "two cold builds of " << c.epub << " diverged";
  const fs::path goldenPath = fs::path(GOLDEN_DIR) / (stem(c.epub) + variantSuffix(c) + ".golden.txt");

  if (std::getenv("UPDATE_GOLDENS")) {
    std::ofstream(goldenPath) << dump;
    GTEST_SKIP() << "golden regenerated: " << goldenPath;
  }
  std::string golden;
  ASSERT_TRUE(readFile(goldenPath, golden))
      << "missing golden " << goldenPath << " — run with UPDATE_GOLDENS=1 to create it";
  EXPECT_EQ(golden, dump) << "layout drift vs golden for " << c.epub << " (" << variantName(c.variant)
                          << ") — if intentional, regenerate with UPDATE_GOLDENS=1 and explain in the commit";

  // A variant is only worth its golden while it still changes this book's layout.
  if (c.variant != Variant::Default) {
    std::string defaultGolden;
    ASSERT_TRUE(readFile(fs::path(GOLDEN_DIR) / (stem(c.epub) + ".golden.txt"), defaultGolden));
    EXPECT_NE(defaultGolden, dump) << c.epub << " no longer lays out differently with " << variantName(c.variant)
                                   << " ON; drop it from its book list and delete its " << variantSuffix(c)
                                   << " golden";
  }
}

template <size_t N>
bool listed(const char* const (&books)[N], const std::string& name) {
  return std::find(std::begin(books), std::end(books), name) != std::end(books);
}

// Every corpus book in the default variant, plus the other variants for the books they cover.
std::vector<Case> corpusCases() {
  std::vector<std::string> paths;
  for (const auto& entry : fs::directory_iterator(CORPUS_DIR)) {
    if (entry.path().extension() == ".epub") paths.push_back(entry.path().string());
  }
  std::sort(paths.begin(), paths.end());

  std::vector<Case> cases;
  for (const auto& path : paths) {
    cases.push_back(Case{path});
    if (listed(kNormalizationSensitiveBooks, stem(path))) cases.push_back(Case{path, Variant::Normalization});
    if (listed(kHyphenationBooks, stem(path))) cases.push_back(Case{path, Variant::Hyphenation});
    if (listed(kBionicBooks, stem(path))) cases.push_back(Case{path, Variant::Bionic});
    if (listed(kFontFamilyBooks, stem(path))) {
      cases.push_back(Case{path, Variant::Accents});
      cases.push_back(Case{path, Variant::AllText});
    }
  }
  return cases;
}

INSTANTIATE_TEST_SUITE_P(SyntheticCorpus, EpubPipelineTest, testing::ValuesIn(corpusCases()),
                         [](const testing::TestParamInfo<Case>& info) {
                           std::string name = stem(info.param.epub) + variantSuffix(info.param);
                           for (char& c : name)
                             if (!isalnum(static_cast<unsigned char>(c))) c = '_';
                           return name;
                         });

}  // namespace
