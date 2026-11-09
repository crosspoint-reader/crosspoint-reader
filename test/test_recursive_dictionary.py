"""Run production dictionary navigation and plain-page preparation with bounded host stubs.

Run: python3 test/test_recursive_dictionary.py (requires a C++20 compiler).
"""
from pathlib import Path
import re
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
    "bool DictionaryDefinitionActivity::prepareFont",
    "void DictionaryDefinitionActivity::showMessage",
    "bool DictionaryDefinitionActivity::restoreCurrentDefinition",
    "void DictionaryDefinitionActivity::navigateDefinition",
    "bool DictionaryDefinitionActivity::buildPlainSelectionPages",
    "void DictionaryDefinitionActivity::drawBody",
    "void DictionaryDefinitionActivity::startSelection",
))
structs = "\n".join(block(header, f"struct {name} {{", True) for name in (
    "Line", "BodyArea", "Definition", "HistoryEntry", "LookupState"
))
location = block((ROOT / "src/util/Dictionary.h").read_text(), "struct DictLocation {", True)
entry = block((ROOT / "src/util/Dictionary.h").read_text(), "struct DictEntry {", True)
entry_list = block((ROOT / "src/util/Dictionary.h").read_text(), "class DictEntryList {", True)
marker = next(line for line in (ROOT / "src/util/Dictionary.h").read_text().splitlines()
              if "DICTIONARY_PLAIN_HEADWORD_MARKER =" in line)
selection = block(selector, "struct Selection {", True)
plain_scratch = block(viewer, "struct DictionaryDefinitionActivity::PlainSelectionLine {", True).replace(
    "GfxRenderer::TextMeasureMode", "Renderer::TextMeasureMode")
constants = viewer.split("namespace {", 1)[1].split("struct PlainLineLayout {", 1)[0]
layout_helpers = "struct PlainLineLayout {" + viewer.split("struct PlainLineLayout {", 1)[1].split("}  // namespace", 1)[0]
layout_helpers = layout_helpers.replace("GfxRenderer", "Renderer").replace(
    "void drawJustifiedPlainLine(", "[[maybe_unused]] void drawJustifiedPlainLine(")
# The harness copies every constant but only some methods; clang's -Wall flags the unused ones.
constants = re.sub(r"^constexpr ", "[[maybe_unused]] constexpr ", constants, flags=re.MULTILINE)
viewer_loop = block(viewer, "void DictionaryDefinitionActivity::loop()")
back_handler = viewer_loop.split("  int sx = 0;", 1)[1].split(
    "  if (lookup && mappedInput.wasReleased(MappedInputManager::Button::Confirm))", 1
)[0]
back_handler = "  int sx = 0;" + back_handler
lookup_source = (ROOT / "src/activities/reader/DictionaryWordSelectActivity.cpp").read_text()
definition_result = block(lookup_source, "[this](const ActivityResult& result)")
reader_source = (ROOT / "src/activities/reader/EpubReaderActivity.cpp").read_text()
return_selection_for = block(reader_source, "ClipSelectionActivity::Selection lookupReturnSelectionFor")
lookup_result = block(reader_source.split("startActivityForResult(std::move(lookup)", 1)[1],
                      "[this](const ActivityResult& result)")
reader_constants = (ROOT / "src/activities/reader/ReaderUtils.h").read_text().split("namespace ReaderUtils {", 1)[1].split(
    "constexpr unsigned long SKIP_HOLD_MS", 1
)[0]
harness = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
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
namespace BidiUtils { enum class BidiBaseDir : signed char { AUTO = -1, LTR = 0, RTL = 1, VISUAL = 2 }; }
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
  struct VisualFragment { uint16_t wordIndex; int16_t x, width; std::string text;
                          int16_t logicalStartX = 0, logicalEndX = 0; };
  uint16_t selectionRowStep = 0;
  void setSelectionRowStep(uint16_t step) { selectionRowStep = step; }
  uint16_t getSelectionRowStep() const { return selectionRowStep; }
  std::vector<VisualFragment> visualFragments;
  bool setVisualFragments(std::vector<VisualFragment>&& spans) {
    visualFragments = std::move(spans);
    return true;
  }
  const std::vector<VisualFragment>* getVisualFragments() const { return &visualFragments; }
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
  int x, y;
  PageLine(std::unique_ptr<TextBlock> block, int x, int y) : block(std::move(block)), x(x), y(y) {}
};
struct Page {
  std::vector<std::unique_ptr<PageElement>> elements;
  template<typename R> void render(R&, int, int, int) const {}
};
struct FontCacheManager {
  bool released = false;
  bool scanning = false;
  void releaseSdFontCaches() { released = true; }
};
struct Renderer {
  FontCacheManager cache;
  size_t maxPreparedBytes = 0;
  size_t prepareCalls = 0;
  int preparedStyles = 0;
  FontCacheManager* getFontCacheManager() { return &cache; }
  bool isFontCacheScanning() const { return cache.scanning; }
  mutable size_t scannedRenderedMeasurements = 0;
  mutable size_t layoutMeasurements = 0;
  enum class TextMeasureMode { Layout, Rendered };
  mutable size_t visualRenderCalls = 0;
  int getTextAdvanceX(int, const char* text, EpdFontFamily::Style style, int8_t = 0,
                      BidiUtils::BidiBaseDir baseDir = BidiUtils::BidiBaseDir::AUTO,
                      TextMeasureMode mode = TextMeasureMode::Layout) const {
    if (baseDir == BidiUtils::BidiBaseDir::VISUAL && mode == TextMeasureMode::Rendered) ++visualRenderCalls;
    if (cache.scanning && mode == TextMeasureMode::Rendered) ++scannedRenderedMeasurements;
    if (mode == TextMeasureMode::Layout) ++layoutMeasurements;
    std::string visual;
    if (baseDir != BidiUtils::BidiBaseDir::VISUAL && BidiUtils::applyBidiVisual(text, visual))
      text = visual.c_str();
    const auto* ptr = reinterpret_cast<const unsigned char*>(text);
    int width = 0;
    int boundaryKern = 0;
    uint32_t previous = 0;
    while (*ptr) {
      const uint32_t cp = utf8NextCodepoint(&ptr);
      ++width;
      // Simulated SD-font kerning between a separator and an RTL glyph.
      if (style == EpdFontFamily::BOLD && mode == TextMeasureMode::Rendered &&
          previous == ' ' && (cp == 'z' || (cp >= 0x590 && cp <= 0x8FF) ||
                                  (cp >= 0xFB50 && cp <= 0xFEFF)))
        boundaryKern += 2;
      previous = cp;
    }
    return width * (style == EpdFontFamily::BOLD ? (mode == TextMeasureMode::Rendered ? 2 : 3) : 1) +
           boundaryKern;
  }
  bool isSdCardFont(int) const { return true; }
  void ensureSdCardFontReady(int, const char* text, int styles) {
    cache.released = false;
    maxPreparedBytes = std::max(maxPreparedBytes, std::strlen(text));
    ++prepareCalls;
    preparedStyles = styles;
  }
  int getSpaceWidth(int, EpdFontFamily::Style) const { return 1; }
  int getLineHeight(int, float compression) const {
    return compression < 1.0f ? 8 : (compression > 1.4f ? 14 : 10);
  }
  struct DrawCall { int x, y; std::string text; EpdFontFamily::Style style;
                    BidiUtils::BidiBaseDir direction; };
  mutable std::vector<DrawCall> drawCalls;
  void drawText(int, int x, int y, const char* text, bool, EpdFontFamily::Style style,
                BidiUtils::BidiBaseDir dir = BidiUtils::BidiBaseDir::AUTO) const {
    drawCalls.push_back({x, y, text, style, dir});
  }
};
struct CrossPointSettings {
  enum { JUSTIFIED, LEFT_ALIGN, CENTER_ALIGN, RIGHT_ALIGN, BOOK_STYLE };
};
struct Settings {
  bool touchReaderControls = true;
  int paragraphAlignment = CrossPointSettings::JUSTIFIED;
  bool extraParagraphSpacing = false;
  float lineCompression = 1.2f;
  float getReaderLineCompression() const { return lineCompression; }
  int getReaderFontId() const { return 0; }
} SETTINGS;
''' + layout_helpers + r'''
struct MappedInputManager {
  enum class Button { Back };
  bool touch = false;
  bool hasTouch() const { return touch; }
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
  bool lookup(const char* word, std::string& text, std::string& headword, LookupResult* result) {
    lastQuery = word;
    if (std::strcmp(word, "missing") == 0) { *result = LookupResult::NotFound; return false; }
    // A synonym selects entry 20, although both entries have the displayed headword "same".
    // The .syn path shows the word that was looked up, the .idx path the stored
    // headword — so looking the displayed headword up again finds the same entry.
    const bool synonym = std::strcmp(word, "alias") == 0;
    const DictLocation location{synonym ? uint32_t{20} : uint32_t{10}, 10, true, false};
    headword = synonym ? word : "same";
    return readDefinition(location, text, result);
  }
  std::string lastQuery;
};
struct ClipSelectionActivity {
''' + selection + r'''
  template<typename... Args> explicit ClipSelectionActivity(Args&&...) {}
};
''' + return_selection_for + r'''
bool styledAvailable = false;
bool buildDictionaryHtmlPages(Renderer& renderer, const std::string&, uint16_t, uint16_t,
                               std::vector<std::unique_ptr<Page>>& pages) {
  renderer.cache.released = false;
  if (!styledAvailable) return false;
  for (int i = 0; i < 4; ++i) pages.push_back(std::make_unique<Page>());
  return true;
}
std::string htmlToPlainText(std::string text, char) { return text; }
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
  ClipSelectionActivity::Selection pendingLookupSelection;
  ClipSelectionActivity::Selection lookupReturnSelection;
  ActivityResult result;
  std::unique_ptr<ClipSelectionActivity> child;
  ActivityResultHandler callback;
  int bodyWidth = 12, bodyHeight = 20;
  BodyArea bodyArea() const { return {0, 0, bodyWidth, bodyHeight}; }
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
  bool prepareFont(std::string&, int);
  int measureSpan(int, const char*, size_t, EpdFontFamily::Style = EpdFontFamily::REGULAR) const;
  void showMessage(StrId);
  void navigateDefinition(bool);
  bool restoreCurrentDefinition(bool);
  bool buildPlainSelectionPages();
  void drawBody(int fontId, int x, int startY, int currentPage) const;
  void startSelection(int x = -1, int y = -1);
};
''' + methods + r'''
int main() {
  {
    DictionaryDefinitionActivity rtlView;
    std::string rtl;
    for (size_t i = 0; i < 1000; ++i) rtl += "\xD8\xA8";
    const std::string original = rtl;
    assert(rtlView.prepareFont(rtl, 0));
    assert(rtl == original && rtlView.renderer.maxPreparedBytes <= MAX_LINE_BYTES);
    assert(rtlView.renderer.prepareCalls > 1 && rtlView.renderer.preparedStyles == 0x03);
    HalMemory::available = 0;
    assert(!rtlView.prepareFont(rtl, 0) && rtl == original);
    HalMemory::available = 1024 * 1024;
  }
  Dictionary dictionary;
  dictionary.entries = {{10, "first entry one two three four five six"}, {20, "other definition"}};
  DictionaryDefinitionActivity view;
  view.lookup = std::make_unique<DictionaryDefinitionActivity::LookupState>(dictionary);
  view.lookup->query = "same";
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
  assert(view.lookup->depth == 1 && view.content.text == dictionary.entries.at(20));
  assert(view.content.text == "other definition");
  dictionary.failRead = true;
  view.navigateDefinition(true);
  assert(view.finished && view.lookup->depth == 1);
  view.finished = false;
  dictionary.failRead = false;
  view.navigateDefinition(true);
  assert(view.lookup->depth == 0 && view.content.text == dictionary.entries.at(10));
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
    assert(!view.finished && view.lookup->depth == 0 && view.content.text == dictionary.entries.at(10));
    assert(view.content.text == dictionary.entries.at(10) && view.content.currentPage == 1);
    assert(view.content.selection.valid);
  }
  // If neither the new entry nor recovery can be laid out, leave lookup safely.
  HalMemory::available = 0;
  view.lookup->pendingWord = "alias";
  view.navigateDefinition(false);
  assert(view.lookup->depth == 0 && view.content.text == dictionary.entries.at(10) && view.content.currentPage == 1);
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
  assert(view.lookup->depth == 0 && view.content.text == dictionary.entries.at(10) && view.content.currentPage == 1);
  // Going back replays the synonym query.
  view.content = {};
  view.lookup->query = "alias";
  view.content.headword = "alias";
  view.content.text = dictionary.entries.at(20);
  assert(view.prepareDefinition(view.content));
  view.lookup->pendingWord = "same";
  view.navigateDefinition(false);
  assert(view.content.text == dictionary.entries.at(10));
  view.navigateDefinition(true);
  assert(view.content.text == dictionary.entries.at(20));
  // Back must exit if the original styled position cannot be restored.
  view.htmlDefinition = styledAvailable = true;
  view.content.text = dictionary.entries.at(10);
  assert(view.prepareDefinition(view.content));
  view.lookup->pendingWord = "alias";
  view.navigateDefinition(false);
  styledAvailable = false;
  view.navigateDefinition(true);
  // The styled path hands the text to the pages, and a failed restore releases
  // the payload, so only the history depth and the headword survive here.
  assert(view.lookup->depth == 1 && view.content.headword == "alias");
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
  view.content.text = "one אבג דהו tail";
  assert(view.prepareDefinition(view.content) && view.buildPlainSelectionPages());
  assert(view.content.lines[0].wraps);
  const auto& mixed = *static_cast<const PageLine&>(*view.lookup->selectionPages[0]->elements[0]).block;
  assert(mixed.words == std::vector<std::string>({"one", "דהו", "אבג"}));
  assert(mixed.positions == std::vector<int16_t>({0, 4, 8}));
  assert(mixed.ranges[1].start > mixed.ranges[2].start);
  view.lookup->selectionPages.clear();
  {
    DictionaryDefinitionActivity spacedRtl;
    spacedRtl.lookup = std::make_unique<DictionaryDefinitionActivity::LookupState>(dictionary);
    spacedRtl.bodyWidth = 14;
    spacedRtl.content.text = "one   אבג  דהו tail";
    assert(spacedRtl.prepareDefinition(spacedRtl.content) && spacedRtl.buildPlainSelectionPages());
    assert(spacedRtl.content.lines[0].wraps);
    const auto& visual = *static_cast<const PageLine&>(*spacedRtl.lookup->selectionPages[0]->elements[0]).block;
    assert(visual.words == std::vector<std::string>({"one", "דהו", "אבג"}));
    assert(visual.positions == std::vector<int16_t>({0, 6, 11}));
    std::string rendered;
    assert(BidiUtils::applyBidiVisual("one   אבג  דהו", rendered));
    assert(rendered == "one   והד  גבא");
    spacedRtl.lookup->selectionPages.clear();
    spacedRtl.content = {};
    spacedRtl.bodyWidth = 20;
    spacedRtl.content.text = "one   אבג  דהו    tail padding";
    assert(spacedRtl.prepareDefinition(spacedRtl.content) && spacedRtl.buildPlainSelectionPages());
    assert(spacedRtl.content.lines[0].wraps);
    const auto& both = *static_cast<const PageLine&>(*spacedRtl.lookup->selectionPages[0]->elements[0]).block;
    assert(both.words == std::vector<std::string>({"one", "דהו", "אבג", "tail"}));
    assert(both.positions == std::vector<int16_t>({0, 6, 11, 18}));
    assert(BidiUtils::applyBidiVisual("one   אבג  דהו    tail", rendered));
    assert(rendered == "one   והד  גבא    tail");

    // A headword control marker inside a separator is invisible to drawBody.
    DictionaryDefinitionActivity::PlainSelectionLine marked;
    marked.text = std::string("one ") + DICTIONARY_PLAIN_HEADWORD_MARKER + "  אבג  דהו";
    marked.layout(spacedRtl, 0, 0, false, {});
    assert(marked.words == std::vector<std::string>({"one", "דהו", "אבג"}));
    assert(marked.positions == std::vector<int16_t>({0, 6, 11}));

    // Bold mixed BiDi text must use the complete rendered-prefix advances,
    // including SD-font kerning across the preceding spaces.
    spacedRtl.lookup->selectionPages.clear();
    spacedRtl.content = {};
    spacedRtl.bodyWidth = 30;
    spacedRtl.content.text = std::string(1, DICTIONARY_PLAIN_HEADWORD_MARKER) + "one   אבג  דהו tail";
    assert(spacedRtl.prepareDefinition(spacedRtl.content) && spacedRtl.buildPlainSelectionPages());
    assert(spacedRtl.content.lines[0].bold && spacedRtl.content.lines[0].wraps);
    const auto& boldMixed = *static_cast<const PageLine&>(*spacedRtl.lookup->selectionPages[0]->elements[0]).block;
    assert(boldMixed.words == std::vector<std::string>({"one", "דהו", "אבג"}));
    assert(boldMixed.positions == std::vector<int16_t>({0, 14, 26}));
    const auto& font = spacedRtl.renderer;
    const int beforeFirst = font.getTextAdvanceX(0, "one   והד", EpdFontFamily::BOLD, 0,
        BidiUtils::BidiBaseDir::VISUAL, Renderer::TextMeasureMode::Rendered) -
        font.getTextAdvanceX(0, "והד", EpdFontFamily::BOLD, 0,
        BidiUtils::BidiBaseDir::VISUAL, Renderer::TextMeasureMode::Rendered);
    const int beforeSecond = font.getTextAdvanceX(0, "one   והד  גבא", EpdFontFamily::BOLD, 0,
        BidiUtils::BidiBaseDir::VISUAL, Renderer::TextMeasureMode::Rendered) -
        font.getTextAdvanceX(0, "גבא", EpdFontFamily::BOLD, 0,
        BidiUtils::BidiBaseDir::VISUAL, Renderer::TextMeasureMode::Rendered);
    assert(boldMixed.positions[1] == beforeFirst && boldMixed.positions[2] == beforeSecond);

    // Reordering must preserve the marker-joined Lam-Alef as one selectable
    // word and map its shaped glyph back to the original source byte range.
    DictionaryDefinitionActivity::PlainSelectionLine joinedArabic;
    joinedArabic.text = std::string("one بيت ل") + DICTIONARY_PLAIN_HEADWORD_MARKER + "ا tail";
    joinedArabic.layout(spacedRtl, 0, 0, false, {});
    assert(joinedArabic.words == std::vector<std::string>({"one", "لا", "بيت", "tail"}));
    assert(joinedArabic.ranges[1].start == 11 && joinedArabic.ranges[1].end == 16);
    assert(joinedArabic.ranges[1].start > joinedArabic.ranges[2].start);
    assert(joinedArabic.positions == std::vector<int16_t>({0, 4, 6, 10}));
    std::string joinedVisual;
    std::vector<uint16_t> joinedSources;
    assert(BidiUtils::applyBidiVisual("one بيت لا tail", joinedVisual, -1, &joinedSources));
    bool mappedLigature = false;
    auto* cursor = reinterpret_cast<const unsigned char*>(joinedVisual.c_str());
    for (size_t cpIndex = 0; *cursor; ++cpIndex) {
      const uint32_t cp = utf8NextCodepoint(&cursor);
      if (cp == 0xFEFB || cp == 0xFEFC) {
        assert(joinedSources[cpIndex] == 8 || joinedSources[cpIndex] == 9);
        mappedLigature = true;
      }
    }
    assert(mappedLigature);
    // Simulated bold SD kerning at the Arabic glyph boundary distinguishes
    // visual-prefix measurement from the old independent-word fallback.
    joinedArabic.layout(spacedRtl, 0, 0, true, {});
    assert(joinedArabic.positions == std::vector<int16_t>({0, 10, 16, 24}));

    // The comma follows the RTL run's whole-line visual order, while the
    // numeric run moves ahead of the Hebrew word. Selectable x values follow
    // those actual rendered positions, not isolated punctuation measurement.
    DictionaryDefinitionActivity::PlainSelectionLine punctuation;
    punctuation.text = "one אבג, 123 tail";
    const size_t beforeLayoutMeasurements = spacedRtl.renderer.layoutMeasurements;
    punctuation.layout(spacedRtl, 0, 0, false, {});
    assert(spacedRtl.renderer.layoutMeasurements == beforeLayoutMeasurements);
    std::string punctuationVisual;
    assert(BidiUtils::applyBidiVisual(punctuation.text.c_str(), punctuationVisual));
    assert(punctuationVisual == "one 123 ,גבא tail");
    assert(punctuation.words == std::vector<std::string>({"one", "123", "אבג,", "tail"}));
    assert(punctuation.positions == std::vector<int16_t>({0, 4, 8, 13}));
    // Token anchors can stay in source order even when the glyphs within a
    // token move: '(' leads the visual token although it came after Hebrew.
    DictionaryDefinitionActivity::PlainSelectionLine sameOrder;
    sameOrder.text = "one אבג(123) tail";
    std::vector<uint16_t> unchangedOrder;
    spacedRtl.renderer.visualRenderCalls = 0;
    sameOrder.layout(spacedRtl, 0, 0, false, {});
    assert(!BidiUtils::computeVisualWordOrder(sameOrder.words, false, unchangedOrder));
    assert(sameOrder.words == std::vector<std::string>({"one", "אבג(123)", "tail"}));
    assert(sameOrder.positions == std::vector<int16_t>({0, 4, 13}));
    assert(spacedRtl.renderer.visualRenderCalls > 0);
    std::string sameOrderVisual;
    std::vector<uint16_t> sameOrderSources;
    assert(BidiUtils::applyBidiVisual(sameOrder.text.c_str(), sameOrderVisual, -1, &sameOrderSources));
    assert(sameOrderVisual == "one (123)גבא tail");
    assert(sameOrderSources[4] == 11);  // mirrored ')' precedes the Hebrew letters in visual order
    assert(sameOrder.fragments.size() == 1);
    assert(sameOrder.fragments[0].wordIndex == 1);
    assert(sameOrder.fragments[0].text == "(123)גבא");
    DictionaryDefinitionActivity::PlainSelectionLine mappedJustify;
    mappedJustify.text = sameOrder.text;
    mappedJustify.layout(spacedRtl, 0, 0, false, {0, 4, 2});
    assert(mappedJustify.positions == std::vector<int16_t>({0, 6, 17}));

    // A mixed-script logical word may occupy disjoint visual intervals.
    DictionaryDefinitionActivity::PlainSelectionLine fragmented;
    fragmented.text = "abcאבג דהו tail";
    fragmented.layout(spacedRtl, 0, 0, false, {});
    assert(fragmented.words == std::vector<std::string>({"abcאבג", "דהו", "tail"}));
    assert(fragmented.positions == std::vector<int16_t>({0, 3, 11}));
    std::string fragmentedVisual;
    assert(BidiUtils::applyBidiVisual(fragmented.text.c_str(), fragmentedVisual));
    assert(fragmentedVisual == "abcוהד גבא tail");
    std::vector<TextBlock::VisualFragment> firstWordFragments;
    for (const auto& span : fragmented.fragments)
      if (span.wordIndex == 0) firstWordFragments.push_back(span);
    assert(firstWordFragments.size() == 2);
    assert(firstWordFragments[0].x == 0 && firstWordFragments[0].width == 3 &&
           firstWordFragments[0].text == "abc");
    assert(firstWordFragments[1].x == 7 && firstWordFragments[1].width == 3 &&
           firstWordFragments[1].text == "גבא");
    assert(firstWordFragments[0].logicalStartX == 0);
    assert(firstWordFragments[0].logicalEndX == 7);
    // The intervening Hebrew word must NOT be inside either highlight interval.
    const auto fragmentContains = [&](const int x) {
      for (const auto& span : firstWordFragments)
        if (x >= span.x && x < span.x + span.width) return true;
      return false;
    };
    assert(fragmentContains(1) && fragmentContains(8) && !fragmentContains(4));

    // A single RTL glyph and a shaped ligature still have logical start and
    // end edges, even though both endpoints map to the same visual glyph.
    DictionaryDefinitionActivity::PlainSelectionLine rtlSingle;
    rtlSingle.text = "א";
    rtlSingle.layout(spacedRtl, 0, 0, false, {}, 1);
    assert(rtlSingle.fragments.size() == 1);
    assert(rtlSingle.fragments[0].logicalStartX == 1);
    assert(rtlSingle.fragments[0].logicalEndX == 0);

    // If the BiDi workspace limit is exceeded, fall back to the original
    // source-order positions rather than leaving a line with zero coordinates.
    DictionaryDefinitionActivity::PlainSelectionLine bidiFallback;
    bidiFallback.text = std::string(124, 'x') + " אבג דהו";
    std::string unavailableVisual;
    assert(!BidiUtils::applyBidiVisual(bidiFallback.text.c_str(), unavailableVisual));
    bidiFallback.layout(spacedRtl, 0, 0, false, {});
    assert(bidiFallback.words == std::vector<std::string>({std::string(124, 'x'), "אבג", "דהו"}));
    assert(bidiFallback.positions == std::vector<int16_t>({0, 125, 129}));
  }
  view.lookup->selectionPages.clear();
  HalMemory::available = 0;
  assert(!view.buildPlainSelectionPages());
  HalMemory::available = 1024 * 1024;
  dictionary.canRead = {};
  // Justified drawing uses the same word positions as plain-text selection.
  {
    Renderer drawing;
    char justified[] = "one two three";
    const auto placement = getPlainLineLayout(drawing, 0, justified, false, true, 20);
    assert(placement.gaps == 2 && placement.justifyExtra == 7);
    drawJustifiedPlainLine(drawing, 0, 5, 7, justified, false, placement);
    assert(drawing.drawCalls.size() == 3);
    assert(drawing.drawCalls[0].text == "one" && drawing.drawCalls[0].x == 5);
    assert(drawing.drawCalls[1].text == "two" && drawing.drawCalls[1].x == 12);
    assert(drawing.drawCalls[2].text == "three" && drawing.drawCalls[2].x == 20);
    for (const auto& call : drawing.drawCalls) assert(call.y == 7 && call.style == EpdFontFamily::REGULAR);

    // A marker between LTR words is not a drawable glyph or an extra
    // justification gap. The selectable positions must match the draw calls.
    DictionaryDefinitionActivity markedJustified;
    markedJustified.lookup = std::make_unique<DictionaryDefinitionActivity::LookupState>(dictionary);
    markedJustified.bodyWidth = 16;
    markedJustified.content.text = std::string("one ") + DICTIONARY_PLAIN_HEADWORD_MARKER + " two three four";
    assert(markedJustified.prepareDefinition(markedJustified.content) &&
           markedJustified.buildPlainSelectionPages());
    assert(markedJustified.content.lines[0].wraps);
    const auto& selectedLine =
        *static_cast<const PageLine&>(*markedJustified.lookup->selectionPages[0]->elements[0]).block;
    assert(selectedLine.words == std::vector<std::string>({"one", "two", "three"}));
    const auto& span = markedJustified.content.lines[0];
    std::string markedText = markedJustified.content.text.substr(span.start, span.len);
    const auto markedPlacement = getPlainLineLayout(markedJustified.renderer, 0, markedText.c_str(), false, true, 16);
    assert(markedPlacement.gaps == 2);
    drawJustifiedPlainLine(markedJustified.renderer, 0, 0, 0, markedText.data(), false, markedPlacement);
    assert(markedJustified.renderer.drawCalls.size() == selectedLine.positions.size());
    for (size_t i = 0; i < selectedLine.positions.size(); ++i) {
      assert(markedJustified.renderer.drawCalls[i].text == selectedLine.words[i]);
      assert(markedJustified.renderer.drawCalls[i].x == selectedLine.positions[i]);
    }

    // An embedded marker cannot split a shaped/ligature-capable drawable word.
    markedJustified.renderer.drawCalls.clear();
    markedJustified.lookup->selectionPages.clear();
    markedJustified.content = {};
    markedJustified.bodyWidth = 12;
    markedJustified.content.text = std::string("f") + DICTIONARY_PLAIN_HEADWORD_MARKER + "i xy end more";
    assert(markedJustified.prepareDefinition(markedJustified.content) &&
           markedJustified.buildPlainSelectionPages());
    assert(markedJustified.content.lines[0].wraps);
    const auto& joinedLine =
        *static_cast<const PageLine&>(*markedJustified.lookup->selectionPages[0]->elements[0]).block;
    assert(joinedLine.words == std::vector<std::string>({"fi", "xy", "end"}));
    std::string joinedText = markedJustified.content.text.substr(
        markedJustified.content.lines[0].start, markedJustified.content.lines[0].len);
    const auto joinedPlacement = getPlainLineLayout(markedJustified.renderer, 0, joinedText.c_str(), false, true, 12);
    assert(joinedPlacement.gaps == 2);
    drawJustifiedPlainLine(markedJustified.renderer, 0, 0, 0, joinedText.data(), false, joinedPlacement);
    assert(markedJustified.renderer.drawCalls.size() == joinedLine.positions.size());
    for (size_t i = 0; i < joinedLine.positions.size(); ++i) {
      assert(markedJustified.renderer.drawCalls[i].text == joinedLine.words[i]);
      assert(markedJustified.renderer.drawCalls[i].x == joinedLine.positions[i]);
    }
    // An RTL run after five digits must use full-line bidi rendering, not logical-order justification.
    char mixedRtl[] = "one 123456אבג 123456דהו tail";
    assert(getPlainLineLayout(drawing, 0, mixedRtl, false, true, 60).gaps == 0);
    char embeddedRtl[] = "one prefixאבג other";
    assert(getPlainLineLayout(drawing, 0, embeddedRtl, false, true, 60).gaps == 0);
  }
  {
    SETTINGS.paragraphAlignment = CrossPointSettings::JUSTIFIED;
    SETTINGS.extraParagraphSpacing = false;
    DictionaryDefinitionActivity boldView;
    boldView.lookup = std::make_unique<DictionaryDefinitionActivity::LookupState>(dictionary);
    boldView.bodyWidth = 20;
    boldView.content.text = std::string(1, DICTIONARY_PLAIN_HEADWORD_MARKER) + "ab cd ef gh";
    assert(boldView.prepareDefinition(boldView.content));
    assert(boldView.content.lines[0].bold && boldView.content.lines[0].wraps);
    assert(boldView.buildPlainSelectionPages());
    const auto& selected = *static_cast<const PageLine&>(*boldView.lookup->selectionPages[0]->elements[0]).block;
    assert(selected.words == std::vector<std::string>({"ab", "cd", "ef"}));
    assert(selected.positions == std::vector<int16_t>({0, 8, 16}));
    auto& drawing = boldView.renderer;
    assert(drawing.getTextAdvanceX(0, "ab", EpdFontFamily::BOLD) == 6);
    assert(drawing.getTextAdvanceX(0, "ab", EpdFontFamily::BOLD, 0,
                                   BidiUtils::BidiBaseDir::AUTO, Renderer::TextMeasureMode::Rendered) == 4);
    char justified[] = "ab cd ef";
    const auto placement = getPlainLineLayout(drawing, 0, justified, true, true, 20);
    assert(placement.gaps == 2 && placement.justifyExtra == 4);
    drawJustifiedPlainLine(drawing, 0, 0, 0, justified, true, placement);
    assert(drawing.drawCalls.size() == selected.positions.size());
    for (size_t i = 0; i < selected.positions.size(); ++i) {
      assert(drawing.drawCalls[i].text == selected.words[i]);
      assert(drawing.drawCalls[i].x == selected.positions[i]);
      assert(drawing.drawCalls[i].style == EpdFontFamily::BOLD);
    }
  }
  // The origin of a justified word includes the kerning introduced by its
  // preceding space, not just the measured prefix ending before the word.
  {
    SETTINGS.paragraphAlignment = CrossPointSettings::JUSTIFIED;
    Renderer kern;
    char line[] = "aa zb cc";
    const auto placement = getPlainLineLayout(kern, 0, line, true, true, 25, 0);
    assert(placement.gaps == 2 && placement.justifyExtra > 0);
    const int throughWord = renderedPlainPrefixWidth(kern, 0, "aa zb", 5, EpdFontFamily::BOLD, 0);
    const int justWord = renderedPlainPrefixWidth(kern, 0, "zb", 2, EpdFontFamily::BOLD, 0);
    const int expectedZ = throughWord - justWord + placement.justifyExtra / placement.gaps;
    const int oldPrefix = renderedPlainPrefixWidth(kern, 0, "aa ", 3, EpdFontFamily::BOLD, 0);
    assert(expectedZ > oldPrefix + placement.justifyExtra / placement.gaps);
    drawJustifiedPlainLine(kern, 0, 0, 0, line, true, placement);
    assert(kern.drawCalls.size() == 3);
    assert(kern.drawCalls[1].text == "zb" && kern.drawCalls[1].x == expectedZ);

    // Force the source-to-visual mapping fallback with a line beyond MiniBidi's
    // workspace limit. Selection must share the drawing origin (including the
    // preceding space's kerning) and the same justification-gap offsets.
    DictionaryDefinitionActivity owner;
    std::string longLine(125, 'a');
    longLine += " zb cc";
    std::string unavailable;
    assert(!BidiUtils::applyBidiVisual(longLine.c_str(), unavailable));
    const auto longPlacement = getPlainLineLayout(kern, 0, longLine.c_str(), true, true, 350, 0);
    assert(longPlacement.gaps == 2 && longPlacement.justifyExtra > 0);
    DictionaryDefinitionActivity::PlainSelectionLine fallback;
    fallback.text = longLine;
    fallback.layout(owner, 0, 0, true, longPlacement, 0);
    kern.drawCalls.clear();
    drawJustifiedPlainLine(kern, 0, 0, 0, longLine.data(), true, longPlacement);
    assert(fallback.words.size() == kern.drawCalls.size());
    for (size_t i = 0; i < fallback.words.size(); ++i) {
      assert(fallback.words[i] == kern.drawCalls[i].text);
      assert(fallback.positions[i] == kern.drawCalls[i].x);
    }

    // When the same long LTR line is not justified, full-line rendering still
    // includes separator-to-word kerning in every selectable word origin.
    DictionaryDefinitionActivity::PlainSelectionLine plainFallback;
    plainFallback.text = longLine;
    plainFallback.layout(owner, 0, 0, true, {}, 0);
    const size_t wordStart = longLine.find("zb");
    const int expectedPlainX =
        renderedPlainWordOrigin(owner.renderer, 0, longLine.c_str(), wordStart, wordStart + 2,
                                EpdFontFamily::BOLD);
    assert(plainFallback.words.size() == 3);
    assert(plainFallback.positions[1] == expectedPlainX);
    assert(expectedPlainX != renderedPlainPrefixWidth(owner.renderer, 0, longLine.c_str(), wordStart,
                                                      EpdFontFamily::BOLD, 0));
  }
  // Alignment offsets are part of the selectable PageLine, including later pages.
  for (const int alignment : {CrossPointSettings::CENTER_ALIGN, CrossPointSettings::RIGHT_ALIGN}) {
    SETTINGS.paragraphAlignment = alignment;
    DictionaryDefinitionActivity alignView;
    alignView.lookup = std::make_unique<DictionaryDefinitionActivity::LookupState>(dictionary);
    alignView.content.text = "one two\nthree four\nfive";
    assert(alignView.prepareDefinition(alignView.content) && alignView.buildPlainSelectionPages());
    assert(alignView.content.totalPages == 2 && alignView.lookup->selectionPages.size() == 2);
    const auto& firstPage = alignView.lookup->selectionPages[0]->elements;
    const auto& secondPage = alignView.lookup->selectionPages[1]->elements;
    assert(firstPage.size() == 2 && secondPage.size() == 1);
    const int firstX = static_cast<const PageLine&>(*firstPage[0]).x;
    const int secondX = static_cast<const PageLine&>(*firstPage[1]).x;
    const int thirdX = static_cast<const PageLine&>(*secondPage[0]).x;
    if (alignment == CrossPointSettings::CENTER_ALIGN)
      assert(firstX == 2 && secondX == 1 && thirdX == 4);
    else
      assert(firstX == 5 && secondX == 2 && thirdX == 8);
  }
  // Justified Hebrew paragraphs anchor both wrapped and final lines to the right.
  for (const int alignment : {CrossPointSettings::JUSTIFIED, CrossPointSettings::BOOK_STYLE}) {
    SETTINGS.paragraphAlignment = alignment;
    DictionaryDefinitionActivity rtlFinal;
    rtlFinal.lookup = std::make_unique<DictionaryDefinitionActivity::LookupState>(dictionary);
    rtlFinal.bodyWidth = 8;
    rtlFinal.content.text = "אחד שני שלוש";
    assert(rtlFinal.prepareDefinition(rtlFinal.content) && rtlFinal.buildPlainSelectionPages());
    assert(rtlFinal.content.lines.size() == 2);
    assert(rtlFinal.content.lines[0].wraps && !rtlFinal.content.lines[1].wraps);
    const auto& page = rtlFinal.lookup->selectionPages[0]->elements;
    assert(page.size() == 2);
    assert(static_cast<const PageLine&>(*page[0]).x == 1);
    assert(static_cast<const PageLine&>(*page[1]).x == 4);
  }
  // Direction belongs to the paragraph: a numeric or Latin-only final line
  // remains right-anchored, and the next independent paragraph resets to LTR.
  for (const int alignment : {CrossPointSettings::JUSTIFIED, CrossPointSettings::BOOK_STYLE}) {
    SETTINGS.paragraphAlignment = alignment;
    for (const char* lead : {"אבג דהו", "ابج دهو"}) {
      for (const char* ending : {"123", "abc"}) {
        DictionaryDefinitionActivity inherited;
        inherited.lookup = std::make_unique<DictionaryDefinitionActivity::LookupState>(dictionary);
        inherited.bodyWidth = 8;
        inherited.content.text = std::string(lead) + " " + ending + "\nabc def";
        assert(inherited.prepareDefinition(inherited.content) && inherited.buildPlainSelectionPages());
        assert(inherited.content.lines.size() == 3);
        assert(inherited.content.lines[0].wraps && inherited.content.lines[0].paragraphRtl);
        assert(!inherited.content.lines[1].wraps && inherited.content.lines[1].paragraphRtl);
        assert(!inherited.content.lines[2].wraps && !inherited.content.lines[2].paragraphRtl);
        const auto& firstPage = inherited.lookup->selectionPages[0]->elements;
        const auto& nextPage = inherited.lookup->selectionPages[1]->elements;
        assert(firstPage.size() == 2 && nextPage.size() == 1);
        assert(static_cast<const PageLine&>(*firstPage[0]).x == 1);
        assert(static_cast<const PageLine&>(*firstPage[1]).x == 5);
        assert(static_cast<const PageLine&>(*nextPage[0]).x == 0);
        inherited.renderer.drawCalls.clear();
        inherited.drawBody(0, 0, 100, 0);
        assert(inherited.renderer.drawCalls.size() == 2);
        for (const auto& drawn : inherited.renderer.drawCalls)
          assert(drawn.direction == BidiUtils::BidiBaseDir::RTL);
        inherited.renderer.drawCalls.clear();
        inherited.drawBody(0, 0, 100, 1);
        assert(inherited.renderer.drawCalls.size() == 1);
        assert(inherited.renderer.drawCalls[0].direction == BidiUtils::BidiBaseDir::LTR);
      }
    }
  }
  // Line compression changes both page breaks and the line y coordinates in
  // the selection page and the actual rendered definition.
  for (const auto& [compression, step] :
       std::array<std::pair<float, int>, 3>{{{0.8f, 8}, {1.2f, 10}, {1.6f, 14}}}) {
    SETTINGS.lineCompression = compression;
    DictionaryDefinitionActivity dense;
    dense.lookup = std::make_unique<DictionaryDefinitionActivity::LookupState>(dictionary);
    dense.bodyHeight = 29;
    dense.content.text = "one\ntwo\nthree\nfour";
    assert(dense.prepareDefinition(dense.content) && dense.buildPlainSelectionPages());
    assert(dense.content.linesPerPage == 29 / step);
    const auto& firstRows = dense.lookup->selectionPages[0]->elements;
    assert(firstRows.size() >= 2);
    assert(static_cast<const PageLine&>(*firstRows[0]).y == 0);
    assert(static_cast<const PageLine&>(*firstRows[1]).y == step);
    dense.renderer.drawCalls.clear();
    dense.drawBody(0, 0, 100, 0);
    assert(dense.renderer.drawCalls.size() == firstRows.size());
    assert(dense.renderer.drawCalls[0].y == 100);
    assert(dense.renderer.drawCalls[1].y == 100 + step);
  }
  SETTINGS.lineCompression = 1.2f;
  // The scan pass must record full visible lines without asking for rendered
  // glyph widths; those are only safe once the SD font has been prewarmed.
  {
    DictionaryDefinitionActivity scanView;
    scanView.bodyWidth = 12;
    scanView.content.text = "alpha beta gamma delta";
    assert(scanView.prepareDefinition(scanView.content));
    assert(scanView.content.lines[0].wraps);
    scanView.renderer.cache.scanning = true;
    scanView.drawBody(0, 0, 0, 0);
    assert(scanView.renderer.scannedRenderedMeasurements == 0);
    assert(!scanView.renderer.drawCalls.empty());
    assert(scanView.renderer.drawCalls[0].text == "alpha beta");
    const size_t scannedCalls = scanView.renderer.drawCalls.size();
    scanView.renderer.cache.scanning = false;
    scanView.drawBody(0, 0, 0, 0);
    assert(scanView.renderer.drawCalls.size() > scannedCalls);
    assert(scanView.renderer.drawCalls[scannedCalls].text == "alpha");
  }
  SETTINGS.paragraphAlignment = CrossPointSettings::JUSTIFIED;
  SETTINGS.extraParagraphSpacing = true;
  {
    DictionaryDefinitionActivity spaced;
    spaced.lookup = std::make_unique<DictionaryDefinitionActivity::LookupState>(dictionary);
    spaced.bodyHeight = 40;
    spaced.content.text = "intro\n1. one\n2. two\n3. three\n4. four\n5. five\n6. six";
    assert(spaced.prepareDefinition(spaced.content) && spaced.buildPlainSelectionPages());
    assert(spaced.content.linesPerPage == 4 && spaced.content.totalPages == 3);
    assert(spaced.content.lines.size() == 9);
    assert(spaced.content.lines[1].len == 0 && spaced.content.lines[5].len == 0);
    assert(spaced.content.lines[4].len > 0 && spaced.content.lines[8].len > 0);
    assert(spaced.lookup->selectionPages.size() == 3);
    const auto& p0 = spaced.lookup->selectionPages[0]->elements;
    const auto& p1 = spaced.lookup->selectionPages[1]->elements;
    const auto& p2 = spaced.lookup->selectionPages[2]->elements;
    assert(p0.size() == 3 && p1.size() == 3 && p2.size() == 1);
    assert(static_cast<const PageLine&>(*p0[1]).y == 20);
    assert(static_cast<const PageLine&>(*p1[0]).y == 0);
    assert(static_cast<const PageLine&>(*p1[1]).y == 20);
    assert(static_cast<const PageLine&>(*p2[0]).y == 0);
  }
  SETTINGS.extraParagraphSpacing = false;
  // Short Back restores one entry; long Back waits for release and cancels the entire flow.
  for (const unsigned long heldMs : {999ul, 1000ul, 2000ul}) {
    DictionaryDefinitionActivity backView;
    backView.lookup = std::make_unique<DictionaryDefinitionActivity::LookupState>(dictionary);
    backView.lookup->depth = 1;
    backView.lookup->history[0].query = "same";
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
  // Lookup exits reopen book selection only on buttons-only devices.
  for (const bool touch : {false, true}) {
    for (const bool cancelled : {false, true}) {
      for (const bool controlsEnabled : {false, true}) {
        SETTINGS.touchReaderControls = controlsEnabled;
        DictionaryDefinitionActivity lookupParent, bookReader;
        ActivityResult exitResult;
        exitResult.isCancelled = cancelled;
        lookupParent.onDefinitionResult(exitResult);
        bookReader.mappedInput.touch = touch;
        ClippingResult clipped;
        clipped.action = ClippingResult::Action::Lookup;
        clipped.startWordIndex = 3;
        clipped.endWordIndex = 5;
        bookReader.lookupReturnSelection = lookupReturnSelectionFor(clipped);
        bookReader.onLookupResult(lookupParent.result);
        assert(bookReader.selectionOpened == (!touch && !cancelled));
        // The restored range is handed to the reopened selection only when it opens.
        assert(bookReader.pendingLookupSelection.valid == bookReader.selectionOpened);
        if (bookReader.selectionOpened) {
          assert(bookReader.pendingLookupSelection.firstWord == 3 && bookReader.pendingLookupSelection.lastWord == 5);
        }
      }
    }
  }
  // A clipping that spans pages cannot be restored; a single-page one keeps its word range.
  ClippingResult multiPage;
  multiPage.startPageOffset = 0;
  multiPage.endPageOffset = 1;
  multiPage.startWordIndex = 3;
  multiPage.endWordIndex = 5;
  assert(!lookupReturnSelectionFor(multiPage).valid);
  ClippingResult singlePage = multiPage;
  singlePage.endPageOffset = 0;
  const auto restored = lookupReturnSelectionFor(singlePage);
  assert(restored.valid && restored.firstPage == 0 && restored.lastPage == 0 && restored.firstWord == 3 &&
         restored.lastWord == 5);
  // Long Back also exits at the first definition, where there are no ancestors.
  DictionaryDefinitionActivity root;
  root.lookup = std::make_unique<DictionaryDefinitionActivity::LookupState>(dictionary);
  root.mappedInput.released = true;
  root.mappedInput.heldMs = 1000;
  root.handleBack();
  assert(root.finished && root.result.isCancelled);
  // Header long-press exits independently of text-selection settings; body long-press still selects.
  HeaderBackTapTarget::set(4, 4, 32, 32);
  const struct { int x, y; bool header, padded; } longPressTargets[] = {
    {10, 10, true, true},
    {0, 10, false, true},
    {10, 0, false, true},
    {40, 10, false, true},
    {10, 40, false, true},
    {50, 0, false, false},
  };
  for (const bool controlsEnabled : {false, true}) {
    SETTINGS.touchReaderControls = controlsEnabled;
    for (const auto& target : longPressTargets) {
      DictionaryDefinitionActivity touchView;
      touchView.lookup = std::make_unique<DictionaryDefinitionActivity::LookupState>(dictionary);
      touchView.content.text = dictionary.entries.at(10);
      assert(touchView.prepareDefinition(touchView.content));
      touchView.mappedInput.longPress = true;
      touchView.mappedInput.touchX = target.x;
      touchView.mappedInput.touchY = target.y;
      assert(HeaderBackTapTarget::contains(target.x, target.y) == target.padded);
      const unsigned readsBeforeTouch = dictionary.reads;
      touchView.handleBack();
      assert(touchView.mappedInput.contactSuppressed && dictionary.reads == readsBeforeTouch);
      assert(touchView.finished == target.header && touchView.result.isCancelled == target.header);
      assert(bool(touchView.child) == (!target.header && controlsEnabled));
      if (target.header) {
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
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <CjkBreaks.h>
#include <Utf8.h>
#define LOG_ERR(...) ((void)0)
#define LOG_INF(...) ((void)0)
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
''' + location + '\n' + marker + '\n' + entry + '\n' + entry_list + r'''
class Dictionary {
 public:
  enum class LookupResult { Found, NotFound, LowMemory, Decompress, ReadError };
  struct LookupSession {};
  static constexpr uint32_t MAX_DEFINITION_BYTES = 64 * 1024;
  bool hasSyn = false;
  bool htmlDefinitions = false;
  unsigned searches = 0;
  std::unique_ptr<DictEntryList> entryList = std::make_unique<DictEntryList>();
  bool isOpen() const { return true; }
  bool openSession(LookupSession&) { return true; }
  void locateAll(LookupSession&, const char*, DictEntryList&, bool* readError) {
    ++searches;
    if (readError) *readError = false;
  }
  bool locateSynonyms(LookupSession&, const char*, DictEntryList&, bool*) {
    ++searches;
    return false;
  }
  bool readDefinition(DictLocation, std::string&, LookupResult*, bool = false, bool* = nullptr) { return false; }
  static std::string cleanWord(const char*);
  static void stemVariants(const std::string&, std::vector<std::string>&);
  bool lookup(const char*, std::string&, std::string&, LookupResult*);
};
''' + lookup_methods + r'''
int main() {
  Dictionary dictionary;
  std::string definition = "unchanged", headword = "unchanged";
  for (const char* suffix : {"ing", "ed", "ies"}) {
    const std::string phrase = std::string(4096 - std::strlen(suffix), 'a') + suffix;
    auto result = Dictionary::LookupResult::Found;
    HalMemory::heap = {12288, 12288};
    dictionary.searches = 0;
    assert(!dictionary.lookup(phrase.c_str(), definition, headword, &result));
    assert(result == Dictionary::LookupResult::LowMemory && dictionary.searches == 0);
    assert(definition == "unchanged" && headword == "unchanged");
    HalMemory::heap = {65536, 4096};
    assert(!dictionary.lookup(phrase.c_str(), definition, headword, &result));
    assert(result == Dictionary::LookupResult::LowMemory && dictionary.searches == 0);
    HalMemory::heap = {65536, 65536};
    assert(!dictionary.lookup(phrase.c_str(), definition, headword, &result));
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
    subprocess.run(["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "lib/Epub/Epub"), "-I", str(ROOT / "lib/Utf8"),
                    str(path / "lookup.cpp"),
                    str(ROOT / "lib/Utf8/Utf8.cpp"), str(ROOT / "lib/Epub/Epub/CjkBreaks.cpp"),
                    str(ROOT / "lib/Epub/Epub/hyphenation/HyphenationCommon.cpp"),
                    "-o", str(path / "lookup")], check=True)
    subprocess.run([str(path / "lookup")], check=True)
print("Recursive dictionary navigation checks passed")
