// How common each kind of selector is in the EPUBs the tests ship (test/epubs and the Moby-Dick
// fixture), and how many of them the resolver can use. A report, not a pin: it prints one
// CSS_CENSUS line per book with complex selectors and a total, and asserts only that its own
// selector split agrees with the parser's count, so the numbers it prints are the parser's.
#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "../../lib/Epub/Epub/css/CssParser.h"
#include "../../lib/ZipFile/ZipFile.h"
#include "HalStorage.h"

namespace {

const char* const kKinds[] = {"simple",  "compound",  "universal", "descendant", "child",
                              "sibling", "attribute", "pseudo",    "too-long"};

// Mutually exclusive, most exotic feature first: `ul > li:first-child` counts as pseudo.
const char* classify(const std::string& selector) {
  if (selector.size() > 256) return "too-long";
  if (selector.find_first_of("+~") != std::string::npos) return "sibling";
  if (selector.find('[') != std::string::npos) return "attribute";
  if (selector.find(':') != std::string::npos) return "pseudo";
  if (selector.find('>') != std::string::npos) return "child";
  if (selector.find(' ') != std::string::npos) return "descendant";
  if (selector.find('*') != std::string::npos) return "universal";
  size_t markers = 0;
  for (const char c : selector) markers += (c == '.' || c == '#') ? 1 : 0;
  // tag.class and tag#id are one marker; .a.b, p.a.b, #id.a are compounds.
  return markers >= 2 ? "compound" : "simple";
}

std::string normalize(std::string_view s) {
  std::string out;
  bool space = true;
  for (const char c : s) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') {
      if (!space) out.push_back(' ');
      space = true;
    } else {
      out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
      space = false;
    }
  }
  if (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

// Splits a stylesheet into top-level rules the way CssParser::loadFromStream does (comments
// dropped, at-rules and their blocks skipped) and counts the selectors of every rule that has a
// declaration the parser keeps.
void census(const std::string& css, std::map<std::string, size_t>& kinds) {
  std::string text;
  for (size_t i = 0; i < css.size(); ++i) {
    if (css[i] == '/' && i + 1 < css.size() && css[i + 1] == '*') {
      const size_t end = css.find("*/", i + 2);
      if (end == std::string::npos) break;
      i = end + 1;
      continue;
    }
    text.push_back(css[i]);
  }
  size_t pos = 0;
  while (pos < text.size()) {
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
    if (pos >= text.size()) break;
    if (text[pos] == '@') {
      // An at-rule ends at its `;`, or after its block.
      size_t i = pos;
      int depth = 0;
      for (; i < text.size(); ++i) {
        if (text[i] == ';' && depth == 0) break;
        if (text[i] == '{') ++depth;
        if (text[i] == '}' && --depth <= 0) break;
      }
      pos = i + 1;
      continue;
    }
    const size_t open = text.find('{', pos);
    if (open == std::string::npos) break;
    size_t close = open + 1;
    for (int depth = 1; close < text.size() && depth > 0; ++close) {
      if (text[close] == '{') ++depth;
      if (text[close] == '}') --depth;
    }
    const std::string group = text.substr(pos, open - pos);
    const std::string body = text.substr(open + 1, close - open - 2);
    pos = close;
    if (!CssParser::parseInlineStyle(body).defined.anySet()) continue;  // the parser drops these too
    size_t start = 0;
    for (size_t i = 0; i <= group.size(); ++i) {
      if (i != group.size() && group[i] != ',') continue;
      const std::string selector = normalize(std::string_view(group).substr(start, i - start));
      start = i + 1;
      if (!selector.empty()) kinds[classify(selector)]++;
    }
  }
}

}  // namespace

TEST(CssSelectorCensus, CorpusEpubs) {
  // CSS_CENSUS_DIR points the census at another folder of books (a local collection).
  const char* otherDir = std::getenv("CSS_CENSUS_DIR");
  std::vector<std::filesystem::path> books;
  for (const auto& entry : std::filesystem::directory_iterator(otherDir ? otherDir : CORPUS_DIR)) {
    if (entry.path().extension() == ".epub") books.push_back(entry.path());
  }
  if (!otherDir) books.push_back(MOBY_DICK_EPUB);
  std::sort(books.begin(), books.end());

  std::map<std::string, size_t> total;
  uint32_t candidates = 0;
  uint32_t unsupported = 0;
  for (const auto& book : books) {
    const std::string bookPath = book.string();  // ZipFile keeps a reference to it
    ZipFile zip(bookPath);
    std::vector<std::string> sheets;
    const bool listed = zip.streamCentralDirectoryNames([&](const std::string_view name) {
      if (name.size() > 4 && name.substr(name.size() - 4) == ".css") sheets.emplace_back(name);
    });
    ASSERT_TRUE(listed) << book;
    std::map<std::string, size_t> kinds;
    CssParser parser("");
    for (const auto& sheet : sheets) {
      size_t size = 0;
      uint8_t* raw = zip.readFileToMemory(sheet.c_str(), &size, false);
      ASSERT_NE(raw, nullptr) << book << ":" << sheet;
      const std::string css(reinterpret_cast<const char*>(raw), size);
      free(raw);
      census(css, kinds);

      const auto path = std::filesystem::temp_directory_path() / ("csscensus-" + std::to_string(getpid()) + ".css");
      std::ofstream(path, std::ios::binary) << css;
      FsFile file;
      ASSERT_TRUE(Storage.openFileForRead("CSS", path.string().c_str(), file));
      ASSERT_TRUE(parser.loadFromStream(file));
      file.close();
      std::filesystem::remove(path);
    }
    size_t seen = 0;
    for (const auto& [kind, count] : kinds) seen += count;
    EXPECT_EQ(seen, parser.selectorCandidates()) << book.filename() << ": the census split the sheets differently";
    candidates += parser.selectorCandidates();
    unsupported += parser.unsupportedSelectors();
    if (kinds.size() > 1 || (kinds.size() == 1 && kinds.begin()->first != "simple")) {
      std::string line = "CSS_CENSUS " + book.filename().string();
      for (const auto& [kind, count] : kinds) line += " " + kind + "=" + std::to_string(count);
      line += " unsupported=" + std::to_string(parser.unsupportedSelectors());
      line += " complexTableBytes=" + std::to_string(parser.complexTableBytes());
      std::printf("%s\n", line.c_str());
    }
    for (const auto& [kind, count] : kinds) total[kind] += count;
  }
  std::string line = "CSS_CENSUS total books=" + std::to_string(books.size());
  for (const char* kind : kKinds) line += " " + std::string(kind) + "=" + std::to_string(total[kind]);
  line += " candidates=" + std::to_string(candidates) + " unsupported=" + std::to_string(unsupported);
  std::printf("%s\n", line.c_str());
  if (!otherDir) EXPECT_GT(total["descendant"] + total["child"], 0u) << "the corpus has descendant selectors to count";
}
