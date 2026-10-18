"""Run production dictionary navigation and plain-page preparation with bounded host stubs.

Run: python3 test/test_recursive_dictionary.py (requires a C++20 compiler).
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
viewer = (ROOT / "src/activities/reader/DictionaryDefinitionActivity.cpp").read_text()
header = (ROOT / "src/activities/reader/DictionaryDefinitionActivity.h").read_text()
selector = (ROOT / "src/activities/reader/ClipSelectionActivity.h").read_text()


def block(source, signature, semicolon=False):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end + int(semicolon)]


methods = "\n".join(block(viewer, signature) for signature in (
    "bool DictionaryDefinitionActivity::prepareDefinition",
    "bool DictionaryDefinitionActivity::layoutHtmlPages",
    "int DictionaryDefinitionActivity::measureSpan",
    "bool DictionaryDefinitionActivity::reserveLines",
    "bool DictionaryDefinitionActivity::wrapText",
    "void DictionaryDefinitionActivity::showMessage",
    "void DictionaryDefinitionActivity::navigateDefinition",
    "bool DictionaryDefinitionActivity::buildPlainSelectionPages",
    "void DictionaryDefinitionActivity::startSelection",
))
structs = "\n".join(block(header, f"struct {name} {{", True) for name in (
    "Line", "BodyArea", "Definition", "HistoryEntry", "LookupState"
))
location = block((ROOT / "src/util/Dictionary.h").read_text(), "struct DictLocation {", True)
selection = block(selector, "struct Selection {", True)
plain_scratch = block(viewer, "struct DictionaryDefinitionActivity::PlainSelectionLine {", True)
constants = viewer.split("namespace {", 1)[1].split("}  // namespace", 1)[0]
harness = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include <BidiUtils.h>
#include <Utf8.h>
#include <Memory.h>
#include "activities/ActivityResult.h"
#define LOG_ERR(...) ((void)0)

''' + constants + r'''

enum class StrId { STR_DICT_LOOKING_UP, STR_DICT_NOT_FOUND, STR_DICT_LOW_MEMORY, STR_DICT_READ_FAILED,
                   STR_DICT_DECOMPRESS_ERROR, STR_DICT_RESTORE_FAILED, STR_DICT_HISTORY_FULL };
struct RenderLock { ~RenderLock() {} };
unsigned long millis() { return 0; }
struct HalMemory {
  struct Heap { size_t freeBytes = 1024 * 1024, largestBlockBytes = 1024 * 1024; };
  static inline size_t available = 1024 * 1024;
  static Heap getDefaultHeap() { return {available, available}; }
};
struct EpdFontFamily { enum Style { REGULAR }; };
struct BlockStyle { bool isRtl = false; };
struct TextBlock {
  struct SourceRange { uint32_t start, end; };
  struct LinkSpan {};
  std::vector<std::string> words;
  std::vector<int16_t> positions;
  std::vector<SourceRange> ranges;
  TextBlock(const std::vector<std::string>& words, const std::vector<int16_t>& positions,
            const std::vector<EpdFontFamily::Style>&, const std::vector<uint8_t>&,
            const std::vector<uint16_t>&, BlockStyle, std::vector<std::string>, std::vector<LinkSpan>,
            const std::vector<SourceRange>& ranges) : words(words), positions(positions), ranges(ranges) {}
  bool valid() const { return true; }
};
struct PageElement { virtual ~PageElement() = default; };
struct PageLine : PageElement {
  std::unique_ptr<TextBlock> block;
  int y;
  PageLine(std::unique_ptr<TextBlock> block, int, int y) : block(std::move(block)), y(y) {}
};
struct Page { std::vector<std::unique_ptr<PageElement>> elements; };
struct Renderer {
  int getTextAdvanceX(int, const char* text, EpdFontFamily::Style) const {
    const auto* ptr = reinterpret_cast<const unsigned char*>(text);
    int width = 0;
    while (*ptr) { utf8NextCodepoint(&ptr); ++width; }
    return width;
  }
  void ensureSdCardFontReady(int, const char*, int) {}
  int getSpaceWidth(int, EpdFontFamily::Style) const { return 1; }
  int getLineHeight(int, float) const { return 10; }
};
struct Settings { int getReaderFontId() const { return 0; } } SETTINGS;
''' + location + r'''
struct Dictionary {
  enum class LookupResult { Found, NotFound, LowMemory, ReadError, Decompress };
  std::map<uint32_t, std::string> entries;
  bool failRead = false;
  unsigned reads = 0;
  bool readDefinition(DictLocation location, std::string& text, LookupResult* result) {
    ++reads;
    if (failRead) { *result = LookupResult::ReadError; return false; }
    text = entries.at(location.offset);
    return true;
  }
  bool lookup(const char* word, std::string& text, std::string& headword, LookupResult* result,
              DictLocation* location) {
    if (std::strcmp(word, "missing") == 0) { *result = LookupResult::NotFound; return false; }
    // A synonym selects entry 20, although both entries have the displayed headword "same".
    const uint32_t offset = std::strcmp(word, "alias") == 0 ? 20 : 10;
    *location = {offset, 10, true, false};
    headword = "same";
    return readDefinition(*location, text, result);
  }
};
struct ClipSelectionActivity {
''' + selection + r'''
  template<typename... Args> explicit ClipSelectionActivity(Args&&...) {}
};
bool styledAvailable = false;
bool buildDictionaryHtmlPages(Renderer&, const std::string&, uint16_t, uint16_t,
                               std::vector<std::unique_ptr<Page>>& pages) {
  if (!styledAvailable) return false;
  for (int i = 0; i < 4; ++i) pages.push_back(std::make_unique<Page>());
  return true;
}
std::string htmlToPlainText(const std::string& text) { return text; }
struct DictionaryDefinitionActivity {
  struct PlainSelectionLine;
''' + structs + r'''
  Renderer renderer;
  int mappedInput = 0;
  bool htmlDefinition = false;
  Definition content;
  std::unique_ptr<LookupState> lookup;
  StrId message{};
  unsigned long messageTime = 0;
  bool messageVisible = false;
  std::unique_ptr<ClipSelectionActivity> child;
  ActivityResultHandler callback;
  BodyArea bodyArea() const { return {0, 0, 12, 20}; }
  void requestUpdate() {}
  void requestUpdateAndWait() {}
  void startActivityForResult(std::unique_ptr<ClipSelectionActivity> next, ActivityResultHandler handler) {
    child = std::move(next);
    callback = std::move(handler);
  }
  bool prepareDefinition(Definition&, bool forcePlain = false);
  bool layoutHtmlPages(Definition&);
  bool reserveLines(Definition&, size_t);
  bool wrapText(Definition&);
  int measureSpan(int, const char*, size_t) const;
  void showMessage(StrId);
  void navigateDefinition(bool);
  bool buildPlainSelectionPages();
  void startSelection(int x = -1, int y = -1);
};
''' + methods + r'''
int main() {
  Dictionary dictionary;
  dictionary.entries = {{10, "first entry one two three four five six"}, {20, "other definition"}};
  DictionaryDefinitionActivity view;
  view.lookup = std::make_unique<DictionaryDefinitionActivity::LookupState>(dictionary, DictLocation{10, 10, true});
  view.content.headword = "same";
  view.content.text = dictionary.entries.at(10);
  assert(view.prepareDefinition(view.content));
  view.content.currentPage = 1;
  view.content.selectionPage = 0;
  view.content.selection = {0, 1, 2, 0, true};
  view.lookup->pendingWord = "alias";
  view.navigateDefinition(false);
  assert(view.lookup->depth == 1 && view.lookup->location.offset == 20);
  assert(view.content.text == "other definition");
  dictionary.failRead = true;
  view.navigateDefinition(true);
  assert(view.lookup->depth == 1 && view.content.text == "other definition");
  assert(view.message == StrId::STR_DICT_READ_FAILED);
  dictionary.failRead = false;
  view.navigateDefinition(true);
  assert(view.lookup->depth == 0 && view.lookup->location.offset == 10);
  assert(view.content.currentPage == 1 && view.content.selection.valid);
  assert(view.content.selection.lastPage == 1 && view.content.selection.firstWord == 2);
  view.lookup->pendingWord = "missing";
  view.navigateDefinition(false);
  assert(view.lookup->depth == 0 && view.content.currentPage == 1);
  assert(view.message == StrId::STR_DICT_NOT_FOUND);
  // A new definition that cannot be laid out does not replace the current one or add history.
  HalMemory::available = 0;
  view.lookup->pendingWord = "alias";
  view.navigateDefinition(false);
  assert(view.lookup->depth == 0 && view.lookup->location.offset == 10 && view.content.currentPage == 1);
  HalMemory::available = 1024 * 1024;
  // Cycles consume ordinary slots. Refusing lookup 17 must not read SD or change the stack.
  for (int i = 0; i < 16; ++i) { view.lookup->pendingWord = "alias"; view.navigateDefinition(false); }
  assert(view.lookup->depth == 16);
  view.startSelection();
  ClippingResult selected;
  selected.action = ClippingResult::Action::Lookup;
  selected.text = "alias";
  view.child.reset();
  const unsigned reads = dictionary.reads;
  view.callback(ActivityResult{std::move(selected)});
  assert(view.lookup->depth == 16 && view.lookup->pendingWord.empty() && dictionary.reads == reads);
  assert(view.message == StrId::STR_DICT_HISTORY_FULL);
  for (int i = 0; i < 16; ++i) view.navigateDefinition(true);
  assert(view.lookup->depth == 0 && view.lookup->location.offset == 10 && view.content.currentPage == 1);
  // Restoring a synonym target uses its entry location, never the ambiguous displayed headword.
  view.content = {};
  view.content.headword = "same";
  view.content.text = dictionary.entries.at(20);
  view.lookup->location = {20, 10, true};
  assert(view.prepareDefinition(view.content));
  view.lookup->pendingWord = "same";
  view.navigateDefinition(false);
  assert(view.lookup->location.offset == 10);
  view.navigateDefinition(true);
  assert(view.lookup->location.offset == 20 && view.content.text == dictionary.entries.at(20));
  view.lookup->location = {10, 10, true};
  // Styled restoration must fail atomically if only plain layout is now available.
  view.htmlDefinition = styledAvailable = true;
  view.content.text = dictionary.entries.at(10);
  assert(view.prepareDefinition(view.content));
  view.lookup->pendingWord = "alias";
  view.navigateDefinition(false);
  styledAvailable = false;
  view.navigateDefinition(true);
  assert(view.lookup->depth == 1 && view.lookup->location.offset == 20 && !view.content.pages.empty());
  assert(view.message == StrId::STR_DICT_RESTORE_FAILED);
  styledAvailable = true;
  view.navigateDefinition(true);
  assert(view.lookup->depth == 0 && !view.content.pages.empty());
  // A plain-text parent stays plain even when styled layout becomes available later.
  view.content = {};
  view.content.headword = "same";
  view.content.text = dictionary.entries.at(10);
  assert(view.prepareDefinition(view.content, true));
  view.lookup->pendingWord = "alias";
  view.navigateDefinition(false);
  view.navigateDefinition(true);
  assert(view.content.pages.empty());
  // The plain adapter keeps byte ranges, CJK character boundaries, line positions, and page boundaries.
  view.htmlDefinition = false;
  view.content = {};
  view.content.text = "one two\n你好世\nthree four\nfive six\nseven eight\nnine ten\neleven";
  assert(view.prepareDefinition(view.content));
  assert(view.buildPlainSelectionPages() && view.lookup->selectionPages.size() == 3);
  const auto& lines = view.lookup->selectionPages[0]->elements;
  const auto& first = static_cast<const PageLine&>(*lines[0]);
  const auto& cjk = static_cast<const PageLine&>(*lines[1]);
  assert(first.block->words == std::vector<std::string>({"one", "two"}));
  assert(first.block->positions == std::vector<int16_t>({0, 4}));
  assert(cjk.block->words == std::vector<std::string>({"你", "好", "世"}));
  assert(cjk.y == 10 && cjk.block->ranges[0].start == 8 && cjk.block->ranges[1].start == 11);
  view.lookup->selectionPages.clear();
  view.content = {};
  view.content.text = "אחד שני";
  assert(view.prepareDefinition(view.content) && view.buildPlainSelectionPages());
  const auto& rtl = *static_cast<const PageLine&>(*view.lookup->selectionPages[0]->elements[0]).block;
  assert(rtl.words == std::vector<std::string>({"שני", "אחד"}));
  assert(rtl.positions == std::vector<int16_t>({0, 4}));
  assert(rtl.ranges[0].start > rtl.ranges[1].start);
  view.lookup->selectionPages.clear();
  view.content = {};
  view.content.text = "one אבג דהו";
  assert(view.prepareDefinition(view.content) && view.buildPlainSelectionPages());
  const auto& mixed = *static_cast<const PageLine&>(*view.lookup->selectionPages[0]->elements[0]).block;
  assert(mixed.words == std::vector<std::string>({"one", "דהו", "אבג"}));
  assert(mixed.positions == std::vector<int16_t>({0, 4, 8}));
  assert(mixed.ranges[1].start > mixed.ranges[2].start);
  view.lookup->selectionPages.clear();
  HalMemory::available = 0;
  assert(!view.buildPlainSelectionPages());
}
'''

dictionary_source = (ROOT / "src/util/Dictionary.cpp").read_text()
lookup_methods = "\n".join(block(dictionary_source, name) for name in (
    "std::string Dictionary::cleanWord", "void Dictionary::stemVariants", "bool Dictionary::lookup"
))
lookup_harness = r'''
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#define LOG_ERR(...) ((void)0)
constexpr size_t DEFINITION_HEAP_HEADROOM_BYTES = 8192;
struct HalMemory {
  struct Heap { size_t freeBytes, largestBlockBytes; };
  static inline Heap heap{65536, 65536};
  static Heap getDefaultHeap() { return heap; }
};
bool isWordByte(unsigned char c) { return c >= 0x80 || std::isalnum(c) != 0; }
''' + location + r'''
class Dictionary {
 public:
  enum class LookupResult { Found, NotFound, LowMemory, Decompress, ReadError };
  struct LookupSession {};
  bool hasSyn = false;
  unsigned searches = 0;
  bool isOpen() const { return true; }
  bool openSession(LookupSession&) { return true; }
  DictLocation locate(LookupSession&, const char*, std::string*) { ++searches; return {}; }
  DictLocation locateSynonym(LookupSession&, const char*, std::string*) { ++searches; return {}; }
  bool readDefinition(DictLocation, std::string&, LookupResult*) { return false; }
  static std::string cleanWord(const char*);
  static void stemVariants(const std::string&, std::vector<std::string>&);
  bool lookup(const char*, std::string&, std::string&, LookupResult*, DictLocation*);
};
''' + lookup_methods + r'''
int main() {
  Dictionary dictionary;
  std::string definition = "unchanged", headword = "unchanged";
  for (const char* suffix : {"ing", "ed", "ies"}) {
    const std::string phrase = std::string(4096 - std::strlen(suffix), 'a') + suffix;
    auto result = Dictionary::LookupResult::Found;
    DictLocation location{10, 20, true};
    HalMemory::heap = {12288, 12288};
    dictionary.searches = 0;
    assert(!dictionary.lookup(phrase.c_str(), definition, headword, &result, &location));
    assert(result == Dictionary::LookupResult::LowMemory && dictionary.searches == 0 && !location.found);
    assert(definition == "unchanged" && headword == "unchanged");
    HalMemory::heap = {65536, 4096};
    assert(!dictionary.lookup(phrase.c_str(), definition, headword, &result, &location));
    assert(result == Dictionary::LookupResult::LowMemory && dictionary.searches == 0);
    HalMemory::heap = {65536, 65536};
    assert(!dictionary.lookup(phrase.c_str(), definition, headword, &result, &location));
    assert(result == Dictionary::LookupResult::NotFound && dictionary.searches > 1);
  }
}
'''

harness = harness.replace('bool DictionaryDefinitionActivity::prepareDefinition',
                          plain_scratch + '\nbool DictionaryDefinitionActivity::prepareDefinition', 1)
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / "navigation.cpp").write_text(harness)
    subprocess.run(["cc", "-c", str(ROOT / "lib/MiniBidi/minibidi.c"), "-o", str(path / "minibidi.o")], check=True)
    subprocess.run([
        "c++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
        "-I", str(ROOT / "src"), "-I", str(ROOT / "lib/Memory"),
        "-I", str(ROOT / "lib/MiniBidi"), "-I", str(ROOT / "lib/Utf8"),
        "-I", str(ROOT / "test/minibidi_arabic/stubs"),
        str(path / "navigation.cpp"), str(path / "minibidi.o"),
        str(ROOT / "lib/MiniBidi/BidiUtils.cpp"), str(ROOT / "lib/Utf8/Utf8.cpp"),
        "-o", str(path / "navigation"),
    ], check=True)
    subprocess.run([str(path / "navigation")], check=True)
    (path / "lookup.cpp").write_text(lookup_harness)
    subprocess.run(["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror", str(path / "lookup.cpp"),
                    "-o", str(path / "lookup")], check=True)
    subprocess.run([str(path / "lookup")], check=True)
print("Recursive dictionary navigation checks passed")
