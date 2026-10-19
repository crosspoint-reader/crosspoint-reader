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
    "bool DictionaryDefinitionActivity::restoreCurrentDefinition",
    "void DictionaryDefinitionActivity::navigateDefinition",
    "bool DictionaryDefinitionActivity::buildPlainSelectionPages",
    "void DictionaryDefinitionActivity::startSelection",
))
structs = "\n".join(block(header, f"struct {name} {{", True) for name in (
    "Line", "BodyArea", "Definition", "HistoryEntry", "LookupState"
))
location = block((ROOT / "src/util/Dictionary.h").read_text(), "struct DictLocation {", True)
marker = next(line for line in (ROOT / "src/util/Dictionary.h").read_text().splitlines()
              if "DICTIONARY_PLAIN_HEADWORD_MARKER =" in line)
selection = block(selector, "struct Selection {", True)
plain_scratch = block(viewer, "struct DictionaryDefinitionActivity::PlainSelectionLine {", True)
constants = viewer.split("namespace {", 1)[1].split("}  // namespace", 1)[0]
viewer_loop = block(viewer, "void DictionaryDefinitionActivity::loop()")
back_handler = viewer_loop.split("  int sx = 0;", 1)[1].split(
    "  if (lookup && mappedInput.wasReleased(MappedInputManager::Button::Confirm))", 1
)[0]
back_handler = "  int sx = 0;" + back_handler
lookup_source = (ROOT / "src/activities/reader/DictionaryWordSelectActivity.cpp").read_text()
definition_result = block(lookup_source, "[this](const ActivityResult& result)")
reader_source = (ROOT / "src/activities/reader/EpubReaderActivity.cpp").read_text()
lookup_result = block(reader_source.split("startActivityForResult(std::move(lookup)", 1)[1],
                      "[this](const ActivityResult& result)")
reader_constants = (ROOT / "src/activities/reader/ReaderUtils.h").read_text().split("namespace ReaderUtils {", 1)[1].split(
    "constexpr unsigned long SKIP_HOLD_MS", 1
)[0]
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
#include "components/HeaderBackTapTarget.h"
#define LOG_ERR(...) ((void)0)
template<typename... Args> void logDebug(Args&&...) {}
#define LOG_DBG(...) logDebug(__VA_ARGS__)

''' + constants + '\nnamespace ReaderUtils {\n' + reader_constants + '}\n' + marker + '\n' + r'''

enum class StrId { STR_DICT_LOOKING_UP, STR_DICT_NOT_FOUND, STR_DICT_LOW_MEMORY, STR_DICT_READ_FAILED,
                   STR_DICT_DECOMPRESS_ERROR, STR_DICT_RESTORE_FAILED, STR_DICT_HISTORY_FULL };
struct RenderLock { ~RenderLock() {} };
unsigned long millis() { return 0; }
struct HalMemory {
  struct Heap { size_t freeBytes = 1024 * 1024, largestBlockBytes = 1024 * 1024; };
  static inline size_t available = 1024 * 1024;
  static Heap getDefaultHeap() { return {available, available}; }
  static Heap getInternalHeap() { return getDefaultHeap(); }
};
struct EpdFontFamily { enum Style { REGULAR, BOLD }; };
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
struct FontCacheManager {
  bool released = false;
  void releaseSdFontCaches() { released = true; }
};
struct Renderer {
  FontCacheManager cache;
  FontCacheManager* getFontCacheManager() { return &cache; }
  int getTextAdvanceX(int, const char* text, EpdFontFamily::Style) const {
    const auto* ptr = reinterpret_cast<const unsigned char*>(text);
    int width = 0;
    while (*ptr) { utf8NextCodepoint(&ptr); ++width; }
    return width;
  }
  void ensureSdCardFontReady(int, const char*, int) { cache.released = false; }
  int getSpaceWidth(int, EpdFontFamily::Style) const { return 1; }
  int getLineHeight(int, float) const { return 10; }
};
struct Settings {
  bool touchReaderControls = true;
  int getReaderFontId() const { return 0; }
} SETTINGS;
struct MappedInputManager {
  enum class Button { Back };
  bool released = false;
  unsigned long heldMs = 0;
  bool longPress = false;
  bool contactSuppressed = false;
  int touchX = 0, touchY = 0;
  bool wasReleased(Button) const { return released; }
  unsigned long getHeldTime() const { return heldMs; }
  bool wasScreenLongPress(int& x, int& y) {
    if (!longPress) return false;
    longPress = false;
    contactSuppressed = true;
    x = touchX;
    y = touchY;
    return true;
  }
};
''' + location + r'''
struct Dictionary {
  enum class LookupResult { Found, NotFound, LowMemory, ReadError, Decompress };
  std::map<uint32_t, std::string> entries;
  bool failRead = false;
  unsigned failuresRemaining = 0;
  LookupResult failure = LookupResult::ReadError;
  std::function<bool()> canRead;
  unsigned reads = 0;
  bool readDefinition(DictLocation location, std::string& text, LookupResult* result) {
    ++reads;
    if (canRead && !canRead()) { *result = LookupResult::LowMemory; return false; }
    if (failuresRemaining) { --failuresRemaining; *result = failure; return false; }
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
bool buildDictionaryHtmlPages(Renderer& renderer, const std::string&, uint16_t, uint16_t,
                               std::vector<std::unique_ptr<Page>>& pages) {
  renderer.cache.released = false;
  if (!styledAvailable) return false;
  for (int i = 0; i < 4; ++i) pages.push_back(std::make_unique<Page>());
  return true;
}
std::string htmlToPlainText(const std::string& text, char) { return text; }
struct DictionaryDefinitionActivity {
  struct PlainSelectionLine;
''' + structs + r'''
  Renderer renderer;
  MappedInputManager mappedInput;
  bool htmlDefinition = false;
  Definition content;
  std::unique_ptr<LookupState> lookup;
  StrId message{};
  unsigned long messageTime = 0;
  bool messageVisible = false;
  bool finished = false;
  bool selectionOpened = false;
  ActivityResult result;
  std::unique_ptr<ClipSelectionActivity> child;
  ActivityResultHandler callback;
  BodyArea bodyArea() const { return {0, 0, 12, 20}; }
  void requestUpdate() {}
  void requestUpdateAndWait() {}
  void finish() { finished = true; }
  void setResult(ActivityResult&& next) { result = std::move(next); }
  void openDictionaryWordSelect() { selectionOpened = true; }
  void handleBack() { ''' + back_handler + r''' }
  void onDefinitionResult(const ActivityResult& child) { (''' + definition_result + r''')(child); }
  void onLookupResult(const ActivityResult& child) { (''' + lookup_result + r''')(child); }
  void startActivityForResult(std::unique_ptr<ClipSelectionActivity> next, ActivityResultHandler handler) {
    child = std::move(next);
    callback = std::move(handler);
  }
  bool prepareDefinition(Definition&, bool forcePlain = false);
  bool layoutHtmlPages(Definition&);
  bool reserveLines(Definition&, size_t);
  bool wrapText(Definition&);
  int measureSpan(int, const char*, size_t, EpdFontFamily::Style = EpdFontFamily::REGULAR) const;
  void showMessage(StrId);
  void navigateDefinition(bool);
  bool restoreCurrentDefinition(bool);
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
  // On C3 the inflate window must fit before another definition is allocated.
  dictionary.canRead = [&] {
    return view.content.text.empty() && view.content.text.capacity() == std::string{}.capacity() &&
           view.content.pages.capacity() == 0 && view.content.lines.capacity() == 0 &&
           view.renderer.cache.released;
  };
  view.lookup->pendingWord = "alias";
  view.navigateDefinition(false);
  assert(view.lookup->depth == 1 && view.lookup->location.offset == 20);
  assert(view.content.text == "other definition");
  dictionary.failRead = true;
  view.navigateDefinition(true);
  assert(view.finished && view.lookup->depth == 1);
  view.finished = false;
  dictionary.failRead = false;
  view.navigateDefinition(true);
  assert(view.lookup->depth == 0 && view.lookup->location.offset == 10);
  assert(view.content.currentPage == 1 && view.content.selection.valid);
  assert(view.content.selection.lastPage == 1 && view.content.selection.firstWord == 2);
  view.lookup->pendingWord = "missing";
  view.navigateDefinition(false);
  assert(view.lookup->depth == 0 && view.content.currentPage == 1);
  assert(view.message == StrId::STR_DICT_NOT_FOUND);
  assert(view.content.text == dictionary.entries.at(10) && view.content.selection.valid && !view.finished);
  // A transient read/inflate failure restores the current entry without pushing history.
  for (auto failure : {Dictionary::LookupResult::ReadError, Dictionary::LookupResult::LowMemory,
                       Dictionary::LookupResult::Decompress}) {
    dictionary.failure = failure;
    dictionary.failuresRemaining = 1;
    view.lookup->pendingWord = "alias";
    view.navigateDefinition(false);
    assert(!view.finished && view.lookup->depth == 0 && view.lookup->location.offset == 10);
    assert(view.content.text == dictionary.entries.at(10) && view.content.currentPage == 1);
    assert(view.content.selection.valid);
  }
  // If neither the new entry nor recovery can be laid out, leave lookup safely.
  HalMemory::available = 0;
  view.lookup->pendingWord = "alias";
  view.navigateDefinition(false);
  assert(view.lookup->depth == 0 && view.lookup->location.offset == 10 && view.content.currentPage == 1);
  assert(view.finished);
  view.finished = false;
  HalMemory::available = 1024 * 1024;
  assert(view.prepareDefinition(view.content));
  view.content.currentPage = 1;
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
  // Back must exit if the original styled position cannot be restored.
  view.htmlDefinition = styledAvailable = true;
  view.content.text = dictionary.entries.at(10);
  assert(view.prepareDefinition(view.content));
  view.lookup->pendingWord = "alias";
  view.navigateDefinition(false);
  styledAvailable = false;
  view.navigateDefinition(true);
  assert(view.lookup->depth == 1 && view.lookup->location.offset == 20);
  assert(view.finished);
  view.finished = false;
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
  HalMemory::available = 1024 * 1024;
  dictionary.canRead = {};
  // Short Back restores one entry; long Back waits for release and cancels the entire flow.
  for (const unsigned long heldMs : {999ul, 1000ul, 2000ul}) {
    DictionaryDefinitionActivity backView;
    backView.lookup = std::make_unique<DictionaryDefinitionActivity::LookupState>(dictionary, DictLocation{20, 10, true});
    backView.lookup->depth = 1;
    backView.lookup->history[0].location = {10, 10, true};
    backView.content.text = dictionary.entries.at(20);
    assert(backView.prepareDefinition(backView.content));
    backView.mappedInput.heldMs = heldMs;
    const unsigned readsBeforeBack = dictionary.reads;
    backView.handleBack();
    assert(!backView.finished && backView.lookup->depth == 1 && dictionary.reads == readsBeforeBack);
    backView.mappedInput.released = true;
    backView.handleBack();
    if (heldMs < 1000) {
      assert(!backView.finished && backView.lookup->depth == 0);
    } else {
      assert(backView.finished && backView.result.isCancelled && dictionary.reads == readsBeforeBack);
      DictionaryDefinitionActivity parent, reader;
      parent.onDefinitionResult(backView.result);
      assert(parent.finished && parent.result.isCancelled);
      reader.onLookupResult(parent.result);
      assert(!reader.selectionOpened);
    }
  }
  // A non-dictionary viewer still closes normally, even after a long hold.
  DictionaryDefinitionActivity parent, reader;
  parent.mappedInput.released = true;
  parent.mappedInput.heldMs = 2000;
  parent.handleBack();
  assert(parent.finished && !parent.result.isCancelled);
  reader.onLookupResult(parent.result);
  assert(reader.selectionOpened);
  // Long Back also exits at the first definition, where there are no ancestors.
  DictionaryDefinitionActivity root;
  root.lookup = std::make_unique<DictionaryDefinitionActivity::LookupState>(dictionary, DictLocation{10, 10, true});
  root.mappedInput.released = true;
  root.mappedInput.heldMs = 1000;
  root.handleBack();
  assert(root.finished && root.result.isCancelled);
  // Header long-press exits independently of text-selection settings; body long-press still selects.
  HeaderBackTapTarget::set(4, 4, 32, 32);
  for (const bool controlsEnabled : {false, true}) {
    SETTINGS.touchReaderControls = controlsEnabled;
    for (const bool onHeader : {false, true}) {
      DictionaryDefinitionActivity touchView;
      touchView.lookup = std::make_unique<DictionaryDefinitionActivity::LookupState>(dictionary, DictLocation{10, 10, true});
      touchView.content.text = dictionary.entries.at(10);
      assert(touchView.prepareDefinition(touchView.content));
      touchView.mappedInput.longPress = true;
      touchView.mappedInput.touchX = onHeader ? 10 : 50;
      touchView.mappedInput.touchY = onHeader ? 10 : 0;
      const unsigned readsBeforeTouch = dictionary.reads;
      touchView.handleBack();
      assert(touchView.mappedInput.contactSuppressed && dictionary.reads == readsBeforeTouch);
      assert(touchView.finished == onHeader && touchView.result.isCancelled == onHeader);
      assert(bool(touchView.child) == (!onHeader && controlsEnabled));
      if (onHeader) {
        DictionaryDefinitionActivity parent, reader;
        parent.onDefinitionResult(touchView.result);
        reader.onLookupResult(parent.result);
        assert(!reader.selectionOpened);
      }
    }
  }
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
#include <string_view>
#include <vector>
#define LOG_ERR(...) ((void)0)
constexpr size_t DEFINITION_HEAP_HEADROOM_BYTES = 8192;
struct HalMemory {
  struct Heap { size_t freeBytes, largestBlockBytes; };
  static inline Heap heap{65536, 65536};
  static Heap getDefaultHeap() { return heap; }
};
bool isWordByte(unsigned char c) { return c >= 0x80 || std::isalnum(c) != 0; }
struct EspStub {
  size_t getMaxAllocHeap() { return 1024 * 1024; }
};
EspStub ESP;
struct TxtToHtml {
  static size_t escapedHtmlSize(std::string_view text) { return text.size(); }
  static void appendEscapedHtml(std::string& out, std::string_view text) { out.append(text); }
};
''' + location + '\n' + marker + r'''
class Dictionary {
 public:
  enum class LookupResult { Found, NotFound, LowMemory, Decompress, ReadError };
  struct LookupSession {};
  static constexpr uint32_t MAX_DEFINITION_BYTES = 64 * 1024;
  bool hasSyn = false;
  bool htmlDefinitions = false;
  unsigned searches = 0;
  bool isOpen() const { return true; }
  bool openSession(LookupSession&) { return true; }
  DictLocation locate(LookupSession&, const char*, std::string*) { ++searches; return {}; }
  bool locateSynonyms(LookupSession&, const char*, std::vector<DictLocation>&, std::vector<std::string>&, bool*) {
    ++searches;
    return false;
  }
  bool readDefinition(DictLocation, std::string&, LookupResult*, bool = false) { return false; }
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
