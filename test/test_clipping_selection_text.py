"""Run with python3 test/test_clipping_selection_text.py (requires a host C/C++ compiler)."""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "src/activities/reader/ClipSelectionActivity.cpp").read_text()
reader_source = (ROOT / "src/activities/reader/EpubReaderActivity.cpp").read_text()
header = (ROOT / "src/activities/reader/ClipSelectionActivity.h").read_text()
word_box = "struct WordBox {" + header.split("struct WordBox {", 1)[1].split("  };", 1)[0] + "};"
selection_bounds_type = "struct SelectionBounds {" + header.split("struct SelectionBounds {", 1)[1].split("  };", 1)[0] + "};"
touch_members = "  bool touchDragSelecting" + header.split("  bool touchDragSelecting", 1)[1].split(
    "  int lastRenderedSelected", 1
)[0]
touch_processing = "void ClipSelectionActivity::processTouch() {\n  int touchX = 0, touchY = 0;\n  if (touchDragSelecting) {" + source.split(
    "  if (touchDragSelecting) {", 1
)[1].split("  const uint8_t buttons = Input::buttonEdges", 1)[0] + "}\n"
action_helpers = "\n".join(line for line in header.splitlines()
                           if "int firstAction() const" in line or "int actionCount() const" in line or "bool isLookup() const" in line)
clean_word = "const char* cleanWordStart" + source.split("const char* cleanWordStart", 1)[1].split(
    "}  // namespace", 1
)[0]
build_text = "bool ClipSelectionActivity::buildSelectedText" + source.split(
    "bool ClipSelectionActivity::buildSelectedText", 1
)[1].split("void ClipSelectionActivity::confirmSelection", 1)[0]
confirm_selection = "void ClipSelectionActivity::confirmSelection" + source.split(
    "void ClipSelectionActivity::confirmSelection", 1
)[1].split("void ClipSelectionActivity::cancel()", 1)[0]
confirm_source = source.split("void ClipSelectionActivity::confirmSelection", 1)[1]
memory_guard = "  const auto heap = HalMemory::getDefaultHeap();" + confirm_source.split(
    "  const auto heap = HalMemory::getDefaultHeap();", 1
)[1].split("  if (lowMemory ||", 1)[0]

def method(name, next_name, text=source):
    signature = text.index(name)
    start = text.rfind("\n", 0, signature) + 1
    return text[start:text.index(next_name, signature)].rstrip()

extract_words = method("bool ClipSelectionActivity::extractWords", "int ClipSelectionActivity::closestInRow")
clean_draw = method("void ClipSelectionActivity::drawWordClean", "void ClipSelectionActivity::drawWordHighlight")
highlight_draw = method("void ClipSelectionActivity::drawWordHighlight", "bool ClipSelectionActivity::renderIncremental")
selection_draw = method("void ClipSelectionActivity::drawSelection", "void ClipSelectionActivity::render(")
incremental_draw = method("bool ClipSelectionActivity::renderIncremental", "void ClipSelectionActivity::drawSelection")
selection_bounds = method("TextBlock::SourceRange ClipSelectionActivity::wholeWordRange",
                          "int ClipSelectionActivity::nextPageStartIndexForTouchDrag")
page_continuation = method("bool pagesShareHyphenatedWord", "// Only single-page selections", reader_source)
select_touch = method("void ClipSelectionActivity::selectTouchIndex", "void ClipSelectionActivity::moveToPage")
word_font = method("int clippingWordFont", "Rect clippingWordRect", reader_source)
word_rect = method("Rect clippingWordRect", "int clampPercent", reader_source)
saved_hit = method("int EpubReaderActivity::clippingAtPoint", "void EpubReaderActivity::drawClippingHighlights", reader_source)
saved_draw = method("void EpubReaderActivity::drawClippingHighlights", "void EpubReaderActivity::renderStatusBar", reader_source)

# Compile production extraction, export, and redraws with bounded UI/rendering stubs.
harness = r"""
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <vector>
#include <BidiUtils.h>
#include <Memory.h>
#include <Utf8.h>
#include <Logging.h>
#undef LOG_DBG
#define LOG_DBG(module, ...) (void)std::snprintf(nullptr, 0, __VA_ARGS__)
#include "clippings/ClippingText.h"
#include "ClippingStore.h"
#include "activities/ActivityResult.h"
#include <Epub/ReaderRenderSpec.h>
namespace BidiUtils { enum class BidiBaseDir : int8_t { AUTO = -1, LTR = 0, RTL = 1 }; }
struct EpdFontFamily { enum Style { REGULAR, UNDERLINE = 4 }; };
constexpr int TAG_PageLine = 1;
struct Rect { int x = 0, y = 0, width = 0, height = 0; };
enum class Color { LightGray };
struct Renderer;
struct Block {
  struct Style { bool isRtl = false; int8_t characterSpacing = 0; } style;
  struct Range { uint32_t start = UINT32_MAX, end = UINT32_MAX; } range;
  using SourceRange = Range;
  const char* text = "one";
  std::string packedText;
  std::vector<EpdFontFamily::Style> styles;
  bool paragraphStart = true;
  bool discretionaryHyphen = false;
  bool prefixSpace = false;
  bool valid() const { return true; }
  const Style& getBlockStyle() const { return style; }
  int getRubyShift(int) const { return 0; }
  int blockFontId(int fallback) const { return fallback; }
  int wordFontId(uint16_t, int fallback) const { return fallback; }
  int wordYOffset(const Renderer&, int, uint16_t) const { return 0; }
  static int renderFontId(const Renderer&, int fontId, int) { return fontId; }
  uint16_t wordCount() const {
    return packedText.empty() ? (prefixSpace ? 2 : 1) : std::count(packedText.begin(), packedText.end(), '\0');
  }
  const char* wordText(uint16_t i) const {
    if (packedText.empty()) return prefixSpace && i == 0 ? " " : text;
    const char* word = packedText.c_str();
    while (i--) word += std::char_traits<char>::length(word) + 1;
    return word;
  }
  uint16_t wordTextLen(uint16_t i) const { return std::char_traits<char>::length(wordText(i)); }
  EpdFontFamily::Style wordStyle(uint16_t i) const { return styles.empty() ? EpdFontFamily::REGULAR : styles[i]; }
  int wordXpos(uint16_t) const { return 0; }
  Range wordSourceRange(uint16_t i) const {
    return prefixSpace && i == 0 ? Range{range.start, range.start + 1} : range;
  }
  bool wordStartsParagraph(uint16_t i) const { return paragraphStart && i == 0; }
  bool wordHasDiscretionaryHyphen(uint16_t) const { return discretionaryHyphen; }
};
struct PageLine {
  int xPos = 0, yPos = 0;
  Block block;
  int getTag() const { return TAG_PageLine; }
  const Block* getBlock() const { return &block; }
};
struct Page { std::vector<std::unique_ptr<PageLine>> elements; };
struct Renderer {
  enum RenderMode { BW };
  struct Draw { int x, width, tracking; };
  mutable std::vector<Draw> draws;
  mutable std::vector<Draw> fills;
  bool sdFont = false;
  mutable size_t coldMeasurements = 0;
  std::set<std::pair<uint8_t, uint32_t>> advances;
  bool isSdCardFont(int) const { return sdFont; }
  int getFontAscenderSize(int) const { return 12; }
  int getLineHeight(int) const { return 16; }
  RenderMode getRenderMode() const { return BW; }
  void getOrientedViewableTRBL(int* top, int* right, int* bottom, int* left) const {
    *top = *right = *bottom = *left = 0;
  }
  int getTextAdvanceX(int, const char* text, EpdFontFamily::Style style, int8_t tracking = 0) const {
    if (sdFont) {
      const uint8_t* p = reinterpret_cast<const uint8_t*>(text);
      while (const uint32_t cp = utf8NextCodepoint(&p)) {
        if (!advances.count({static_cast<uint8_t>(style) & 3, cp})) ++coldMeasurements;
      }
    }
    const int length = std::char_traits<char>::length(text);
    return length * 8 + (length - 1) * tracking;
  }
  void ensureSdCardFontReady(int, const char*, uint8_t) {}
  void ensureSdCardFontReady(int, const char* const* segments, const size_t* lengths, size_t count,
                             bool, bool, uint8_t mask) {
    for (size_t i = 0; i < count; ++i) {
      const char* word = segments[i];
      const char* end = word + lengths[i];
      while (word < end) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(word);
        while (const uint32_t cp = utf8NextCodepoint(&p)) {
          for (uint8_t style = 0; style < 4; ++style) {
            if (mask & (1 << style)) advances.emplace(style, cp);
          }
        }
        word += std::char_traits<char>::length(word) + 1;
      }
      assert(word == end);
    }
  }
  void drawText(int font, int x, int, const char* text, bool, EpdFontFamily::Style style,
                BidiUtils::BidiBaseDir = BidiUtils::BidiBaseDir::AUTO, int8_t tracking = 0) const {
    draws.push_back({x, getTextAdvanceX(font, text, style, tracking), tracking});
  }
  void fillRect(int x, int, int width, int, bool) const { fills.push_back({x, width, 0}); }
  void fillRectDither(int x, int, int width, int, Color) const { fills.push_back({x, width, 0}); }
  void drawRect(int, int, int, int, bool) const {}
  void drawPixel(int, int, bool) const {}
  void displayBuffer(int) const {}
};
struct HalDisplay { static constexpr int FAST_REFRESH = 0; };
struct Theme {
  void drawSelectionHandle(const Renderer&, Rect, bool) const {}
  void drawSelectionActions(const Renderer&, Rect, int = 3, int = 0) const {}
  void drawButtonHints(const Renderer&, const char*, const char*, const char*, const char*) const {}
} GUI;
struct Input {
  bool held = false, pressed = false;
  int x = 0, y = 0;
  bool hasTouch() const { return false; }
  bool isScreenTouchHeld(int& outX, int& outY) const { outX = x; outY = y; return held; }
  bool wasScreenTouchPressed(int& outX, int& outY) const { outX = x; outY = y; return pressed; }
  struct Labels { const char *btn1, *btn2, *btn3, *btn4; };
  Labels mapLabels(const char* a, const char* b, const char* c, const char* d) const { return {a, b, c, d}; }
};
using GfxRenderer = Renderer;
using TextBlock = Block;
struct BoardConfig { static bool isEegoA4() { return false; } };
unsigned long millis() { return 0; }
struct Settings {
  int screenMargin = 0;
  int getReaderFontId() const { return 0; }
  ReaderRenderSpec readerRenderSpec(int width, int height) const {
    ReaderRenderSpec spec;
    spec.viewportWidth = width;
    spec.viewportHeight = height;
    return spec;
  }
} SETTINGS;
struct SavedClippings {
  Clipping clipping;
  bool hasClippings() const { return true; }
  size_t clippingCount() const { return 1; }
  const Clipping* clippingAt(size_t) const { return &clipping; }
} savedClippings;
#undef CLIPPINGS
#define CLIPPINGS savedClippings
struct HalMemory {
  struct HeapStats { size_t freeBytes, largestBlockBytes; };
  static inline HeapStats stats{};
  static HeapStats getDefaultHeap() { return stats; }
};
bool selectionLowMemory(const int first, const int last) {
""" + memory_guard + r"""
  return lowMemory;
}
""" + clean_word + r"""
enum class StrId { STR_BACK, STR_MEMORY_ERROR, STR_CLIPPING_TOO_LONG };
enum Hint { STR_BACK, STR_SELECT, STR_DIR_LEFT, STR_DIR_RIGHT, STR_LOOKUP, STR_DONE };
const char* tr(Hint) { return ""; }
constexpr int TOUCH_DRAG_MOVEMENT_PX = 4;
constexpr unsigned long TOUCH_PAGE_ADVANCE_HOLD_MS = 1000;
struct ClipSelectionActivity {
""" + word_box + selection_bounds_type + action_helpers + r"""
  std::unique_ptr<WordBox[]> words = makeUniqueNoThrow<WordBox[]>(240);
  static constexpr size_t MAX_SELECTABLE_WORDS = 240;
  enum class Mode { Clip, Lookup, DefinitionLookup };
  Mode mode = Mode::Clip;
  bool lookupAvailable = true;
  Renderer renderer;
  Input mappedInput;
  std::vector<std::unique_ptr<Page>> pages;
  std::span<const std::unique_ptr<Page>> borrowedPages;
  size_t pageCount() const { return borrowedPages.empty() ? pages.size() : borrowedPages.size(); }
  const Page& pageAt(size_t i) const { return *(borrowedPages.empty() ? pages[i] : borrowedPages[i]); }
  size_t wordCount = 0;
  uint16_t rowCount = 0;
  int fontId = 0, lineHeight = 16, marginLeft = 0, marginTop = 0;
  int selected = 0, rangeStart = -1;
  uint8_t currentPageOffset = 0;
""" + touch_members + r"""
  ClippingResult confirmed;
  bool finished = false;
  struct Popup {
    bool active = false;
    bool isActive() const { return active; }
    void show(StrId, const StrId*, int, int, void (*)(int)) { active = true; }
  } actionPopup;
  void setResult(ClippingResult result) { confirmed = std::move(result); }
  void finish() { finished = true; }
  void requestUpdate() {}
  void confirmSelection(ClippingResult::Action);
  void processTouch();
  bool renderIncremental();
  int lastRenderedSelected = -1, lastRenderedRangeStart = -1, lastRenderedPageOffset = -1;
  int lastRenderedTextOffset = 0, lastRenderedTextXOffset = 0;
  int dragHit = -1;
  int dragWordAt(int, int) const { return dragHit; }
  int wordAt(int, int) const { return dragHit; }
  int actionAt(int, int) const { return -1; }
  int nextPageStartIndexForTouchDrag() const { return -1; }
  bool isWithinCurrentPageEndDwellSlop(int, int) const { return false; }
  void selectIndex(int index) { selected = index; currentPageOffset = words[index].pageOffset; }
  void selectTouchIndex(int);
  bool buildSelectedText(int first, int last, std::string& text) const;
  TextBlock::SourceRange wholeWordRange(int index) const;
  SelectionBounds selectionBounds(int anchor, int cursor) const;
  bool extractWords();
  void drawWordClean(int, int, int) const;
  void drawWordHighlight(int, const SelectionBounds&, int, int) const;
  void drawSelection() const;
  void prewarmWord(int) const {}
  void clearGapBetween(const WordBox&, const WordBox&, int, int) const {}
  void ditherGapBetween(const WordBox&, const WordBox&, int, int) const {}
  int textOffset() const { return 0; }
  int textXOffset() const { return 0; }
  Rect handleRect(int, bool start) const { return {start ? 0 : 100, 0, 20, 20}; }
  Rect actionRect() const { return {}; }
};
""" + build_text + confirm_selection + extract_words + clean_draw + highlight_draw + incremental_draw + selection_draw + selection_bounds + page_continuation + select_touch + touch_processing + r"""
struct EpubReaderActivity {
  Renderer& renderer;
  explicit EpubReaderActivity(Renderer& renderer) : renderer(renderer) {}
  struct Section { int currentPage = 0, pageCount = 1; } sectionData;
  Section* section = &sectionData;
  int currentSpineIndex = 0, buildViewportWidth = 480, buildViewportHeight = 800;
  int clippingAtPoint(const Page&, int, int) const;
  void drawClippingHighlights(const Page&, int, int, int) const;
};
""" + word_font + word_rect + saved_hit + saved_draw + r"""
int loadLine(ClipSelectionActivity& activity, const std::vector<std::string>& logical,
             bool rtl, int first = 0, uint32_t offset = 0, uint16_t row = 0) {
  std::vector<uint16_t> visual;
  if (!BidiUtils::computeVisualWordOrder(logical, rtl, visual)) {
    for (size_t i = 0; i < logical.size(); ++i) visual.push_back(i);
  }
  std::vector<uint32_t> starts;
  for (const auto& text : logical) {
    starts.push_back(offset);
    for (const unsigned char c : text) if ((c & 0xc0) != 0x80) ++offset;
    ++offset;
  }
  for (size_t i = 0; i < visual.size(); ++i) {
    const size_t index = visual[i];
    auto& word = activity.words[first + i];
    word = {};
    word.text = logical[index].c_str();
    word.startOffset = starts[index];
    word.endOffset = index + 1 < starts.size() ? starts[index + 1] - 1 : offset - 1;
    word.x = i * 100;
    word.width = 80;
    word.row = row;
    word.pageOffset = row;
    word.isRtl = rtl;
  }
  // extractWords() keeps screen navigation in paragraph direction.
  if (rtl) std::reverse(activity.words.get() + first, activity.words.get() + first + visual.size());
  return first + visual.size() - 1;
}
void expect(const ClipSelectionActivity& activity, int first, int last, const char* expected) {
  std::string text;
  assert(activity.buildSelectedText(first, last, text));
  if (text != expected) std::fprintf(stderr, "Expected: %s\nActual:   %s\n", expected, text.c_str());
  assert(text == expected);
}
void expectBounds(const ClipSelectionActivity& activity, int anchor, int cursor, int first, int last) {
  const auto bounds = activity.selectionBounds(anchor, cursor);
  assert(bounds.first == first && bounds.last == last);
}
int main() {
  {
    ClipSelectionActivity split;
    const char* fragments[] = {"before", "extra-", "ordi-", "nary", "after"};
    for (int i = 0; i < 5; ++i) {
      auto page = std::make_unique<Page>();
      auto line = std::make_unique<PageLine>();
      line->block.text = fragments[i];
      line->block.range = {uint32_t(i == 0 ? 0 : 7 + (i - 1) * 5), uint32_t(i == 0 ? 6 : 12 + (i - 1) * 5)};
      line->block.paragraphStart = i == 0;
      line->block.discretionaryHyphen = i == 1 || i == 2;
      page->elements.push_back(std::move(line));
      split.pages.push_back(std::move(page));
    }
    assert(!pagesShareHyphenatedWord(*split.pages[0], *split.pages[1]));
    assert(pagesShareHyphenatedWord(*split.pages[1], *split.pages[2]));
    assert(pagesShareHyphenatedWord(*split.pages[2], *split.pages[3]));
    assert(!pagesShareHyphenatedWord(*split.pages[3], *split.pages[4]));
    split.pages[2]->elements[0]->block.paragraphStart = true;
    assert(!pagesShareHyphenatedWord(*split.pages[1], *split.pages[2]));
    split.pages[2]->elements[0]->block.paragraphStart = false;
    assert(split.extractWords());
    for (int fragment = 1; fragment <= 3; ++fragment) {
      for (int anchor : {-1, fragment}) {
        const auto bounds = split.selectionBounds(anchor, fragment);
        assert(bounds.first == 1 && bounds.last == 3);
        expect(split, fragment, fragment, "extraordinary");
        split.rangeStart = anchor;
        split.selected = fragment;
        for (int page = 1; page <= 3; ++page) {
          split.currentPageOffset = page;
          split.renderer.draws.clear();
          split.drawSelection();
          assert(split.renderer.draws.size() == 1);
        }
      }
    }
    expectBounds(split, 0, 2, 0, 3);
    expectBounds(split, 4, 2, 1, 4);
    expectBounds(split, -1, 0, 0, 0);
    expectBounds(split, -1, 4, 4, 4);
    split.words[2].paragraphStart = true;
    expectBounds(split, -1, 1, 1, 1);
    split.words[2].paragraphStart = false;
    split.words[1].endOffset = UINT32_MAX;
    expectBounds(split, -1, 1, 1, 1);
    split.words[1].endOffset = split.words[2].startOffset - 1;
    expectBounds(split, -1, 1, 1, 1);
    split.words[1].endOffset = split.words[2].startOffset;
    split.words[1].discretionaryHyphen = false;
    expectBounds(split, -1, 1, 1, 1);  // A literal hyphen is not a layout split.
    expect(split, 1, 3, "extra-ordinary");
  }
  {
    // Per-line RTL reversal separates the fragments visually with another English token.
    ClipSelectionActivity mixed;
    mixed.wordCount = 4;
    const char* text[] = {"extra-", "one", "ordinary", "next"};
    const uint32_t starts[] = {4, 0, 9, 18};
    const uint32_t ends[] = {9, 3, 17, 22};
    for (int i = 0; i < 4; ++i) {
      auto& word = mixed.words[i];
      word.text = text[i];
      word.startOffset = starts[i];
      word.endOffset = ends[i];
      word.row = i / 2;
      word.x = i * 100;
      word.pageWordIndex = i;
      word.isRtl = true;
      word.discretionaryHyphen = i == 0;
    }
    for (int cursor : {0, 2}) {
      for (int anchor : {-1, cursor}) {
        mixed.selected = cursor;
        mixed.rangeStart = anchor;
        const auto bounds = mixed.selectionBounds(anchor, cursor);
        assert(bounds.contains(0, mixed.words[0]));
        assert(!bounds.contains(1, mixed.words[1]));
        assert(bounds.contains(2, mixed.words[2]));
        assert(!bounds.contains(3, mixed.words[3]));
        expect(mixed, cursor, cursor, "extraordinary");
        mixed.renderer.draws.clear();
        mixed.drawSelection();
        assert(mixed.renderer.draws.size() == 2);
      }
      HalMemory::stats = {1024 * 1024, 1024 * 1024};
      mixed.rangeStart = mixed.selected = cursor;
      mixed.confirmSelection(ClippingResult::Action::Lookup);
      assert(mixed.finished && mixed.confirmed.text == "extraordinary");
      assert(mixed.confirmed.wordCount == 2);
      assert(mixed.confirmed.startWordIndex == cursor && mixed.confirmed.endWordIndex == cursor);
      expect(mixed, mixed.confirmed.startWordIndex, mixed.confirmed.endWordIndex, "extraordinary");
      mixed.confirmSelection(ClippingResult::Action::Clip);
      assert(mixed.confirmed.text == "extraordinary" && mixed.confirmed.wordCount == 2);
      assert(mixed.confirmed.startWordIndex == 0 && mixed.confirmed.endWordIndex == 2);
      assert(mixed.confirmed.startOffset == 4 && mixed.confirmed.endOffset == 17);
      for (bool startHandle : {false, true}) {
        for (bool move : {false, true}) {
          mixed.rangeStart = mixed.selected = cursor;
          mixed.mappedInput = {false, true, startHandle ? 5 : 105, 5};
          mixed.processTouch();
          assert(mixed.touchDragSelecting);
          assert(mixed.rangeStart == cursor && mixed.selected == cursor);
          mixed.mappedInput.pressed = false;
          mixed.mappedInput.held = true;
          if (move) mixed.mappedInput.x += 10;
          mixed.dragHit = cursor == 0 ? 2 : 0;
          mixed.processTouch();
          assert(mixed.rangeStart == cursor && mixed.selected == cursor);
          mixed.mappedInput.held = false;
          mixed.processTouch();
          assert(!mixed.touchDragSelecting);
          expect(mixed, mixed.rangeStart, mixed.selected, "extraordinary");
        }
        mixed.rangeStart = mixed.selected = cursor;
        mixed.mappedInput = {false, true, startHandle ? 5 : 105, 5};
        mixed.processTouch();
        mixed.mappedInput.pressed = false;
        mixed.mappedInput.held = true;
        mixed.mappedInput.x += 10;
        mixed.dragHit = 1;
        mixed.processTouch();
        expect(mixed, std::min(mixed.rangeStart, mixed.selected),
               std::max(mixed.rangeStart, mixed.selected), "one extraordinary");
        mixed.mappedInput.held = false;
        mixed.processTouch();
      }
    }
    mixed.rangeStart = mixed.lastRenderedRangeStart = -1;
    mixed.lastRenderedPageOffset = 0;
    mixed.lastRenderedSelected = 3;
    mixed.selected = 0;
    mixed.renderer.draws.clear();
    assert(mixed.renderIncremental());
    assert(mixed.renderer.draws.size() == 3);
    for (const auto& draw : mixed.renderer.draws) assert(draw.x != 100);
    mixed.selected = 3;
    mixed.renderer.draws.clear();
    assert(mixed.renderIncremental());
    assert(mixed.renderer.draws.size() == 3);
    for (const auto& draw : mixed.renderer.draws) assert(draw.x != 100);
    mixed.rangeStart = mixed.lastRenderedRangeStart = 0;
    mixed.lastRenderedSelected = 3;
    mixed.selected = 0;
    mixed.renderer.draws.clear();
    assert(mixed.renderIncremental());
    assert(mixed.renderer.draws.size() == 2);
    expect(mixed, 1, 2, "one extraordinary");
    mixed.rangeStart = mixed.selected = 0;
    mixed.words[2].pageOffset = 1;
    mixed.selectTouchIndex(2);
    assert(mixed.currentPageOffset == 1 && mixed.selected == 0);
    expect(mixed, mixed.rangeStart, mixed.selected, "extraordinary");
    mixed.selectTouchIndex(1);
    assert(mixed.selected == 1 && mixed.currentPageOffset == 0);
    expect(mixed, mixed.rangeStart, mixed.selected, "one extraordinary");
  }
  // Returning from a definition releases the SD font caches before selection is rebuilt.
  for (const bool rtl : {false, true}) {
    ClipSelectionActivity cold;
    cold.renderer.sdFont = true;
    for (int pageIndex = 0; pageIndex < 3; ++pageIndex) {
      auto page = std::make_unique<Page>();
      auto line = std::make_unique<PageLine>();
      line->block.style.isRtl = rtl;
      for (int i = 0; i < 120; ++i) {
        // More than 2048 bytes per line, with a new codepoint at the end.
        line->block.packedText += i == 119 ? "abcdefghijklmnopqrZ" : "abcdefghijklmnopqrs";
        line->block.packedText += '\0';
        line->block.styles.push_back(static_cast<EpdFontFamily::Style>(i % 4));
      }
      page->elements.push_back(std::move(line));
      cold.pages.push_back(std::move(page));
    }
    assert(cold.extractWords());
    assert(cold.wordCount == 240);
    assert(cold.words[0].pageOffset == 0);
    assert(cold.words[239].pageOffset == 1);
    assert(cold.renderer.coldMeasurements == 0);
    cold.mode = ClipSelectionActivity::Mode::DefinitionLookup;
    cold.borrowedPages = cold.pages;
    HalMemory::stats = {1024 * 1024, 1024 * 1024};
    assert(cold.extractWords());
    assert(cold.wordCount == 360 && cold.words[359].pageOffset == 2);
    assert(cold.pages[0] && cold.pages[1] && cold.pages[2]);
    HalMemory::stats = {8192, 4096};
    assert(!cold.extractWords());
  }
  {
    ClipSelectionActivity dense;
    auto page0 = std::make_unique<Page>();
    auto line0 = std::make_unique<PageLine>();
    for (int i = 0; i < 300; ++i) {
      line0->block.packedText += "あ";
      line0->block.packedText += '\0';
      line0->block.styles.push_back(EpdFontFamily::REGULAR);
    }
    page0->elements.push_back(std::move(line0));
    dense.pages.push_back(std::move(page0));

    auto page1 = std::make_unique<Page>();
    auto line1 = std::make_unique<PageLine>();
    for (int i = 0; i < 20; ++i) {
      line1->block.packedText += "い";
      line1->block.packedText += '\0';
      line1->block.styles.push_back(EpdFontFamily::REGULAR);
    }
    page1->elements.push_back(std::move(line1));
    dense.pages.push_back(std::move(page1));

    assert(dense.extractWords());
    assert(dense.wordCount == 310);
    assert(dense.words[299].pageOffset == 0);
    assert(dense.words[300].pageOffset == 1);
    assert(dense.words[309].pageOffset == 1);
    dense.currentPageOffset = 1;  // A preceding page was retained for a split word.
    assert(dense.extractWords());
    assert(dense.wordCount == 320 && dense.words[319].pageOffset == 1);
  }
  HalMemory::stats = {5908, 3444};  // Physical X3 crash workload.
  assert(selectionLowMemory(0, 0));
  HalMemory::stats = {4500, 4200};  // A large enough block still needs spare heap.
  assert(selectionLowMemory(0, 0));
  HalMemory::stats = {8192, 6144};
  assert(!selectionLowMemory(0, 0));
  assert(!selectionLowMemory(0, 239));
  ClipSelectionActivity activity;
  for (const int8_t tracking : {2, -2}) {
    auto page = std::make_unique<Page>();
    for (int i = 0; i < 2; ++i) {
      auto line = std::make_unique<PageLine>();
      line->xPos = i * 80;
      line->yPos = i * 16;
      line->block.style.characterSpacing = tracking;
      line->block.range = {uint32_t(i * 3), uint32_t((i + 1) * 3)};
      line->block.text = i == 0 ? "one" : "two";
      page->elements.push_back(std::move(line));
    }
    activity.pages.clear();
    activity.pages.push_back(std::move(page));
    assert(activity.extractWords());
    assert(activity.wordCount == 2);
    expect(activity, 0, 1, "one\ntwo");
    activity.words[1].startOffset += 3;  // Whitespace-formatted XHTML has an offset gap.
    expect(activity, 0, 1, "one\ntwo");
    activity.pages[0]->elements[1]->block.prefixSpace = true;
    assert(activity.extractWords());
    assert(activity.wordCount == 2);
    expect(activity, 0, 1, "one\ntwo");
    activity.renderer.draws.clear();
    activity.renderer.fills.clear();
    activity.rangeStart = 0;
    activity.selected = 1;
    activity.drawSelection();
    activity.drawWordClean(0, 0, 0);  // Cursor leaves the old word.
    activity.drawWordHighlight(1, activity.selectionBounds(1, 1), 0, 0);
    assert(activity.renderer.draws.size() == 4);
    assert(activity.renderer.fills.size() == 4);
    for (size_t i = 0; i < 4; ++i) {
      const auto& draw = activity.renderer.draws[i];
      const auto& fill = activity.renderer.fills[i];
      assert(draw.tracking == tracking);
      assert(draw.width == 24 + 2 * tracking);
      assert(fill.width == draw.width);
      assert(fill.x == draw.x);
    }
    for (size_t i = 0; i < 2; ++i) {
      assert(activity.words[i].characterSpacing == tracking);
      assert(activity.words[i].width == 24 + 2 * tracking);
    }
    EpubReaderActivity reader{activity.renderer};
    savedClippings.clipping.startOffset = 0;
    savedClippings.clipping.endOffset = 3;
    activity.renderer.fills.clear();
    reader.drawClippingHighlights(*activity.pages[0], 0, 0, 0);
    assert(activity.renderer.fills.size() == 1);
    const int width = activity.words[0].width;
    assert(activity.renderer.fills[0].width == width);
    assert(reader.clippingAtPoint(*activity.pages[0], width - 1, 8) == 0);
    assert(reader.clippingAtPoint(*activity.pages[0], width, 8) == -1);
  }
  for (const auto& logical : {std::vector<std::string>{"אחד", "alpha", "beta", "שני"},
                              std::vector<std::string>{"واحد", "alpha", "beta", "اثنان"}}) {
    loadLine(activity, logical, true);
    const char* visualSecond = activity.words[1].text;
    expect(activity, 0, 3, (logical[0] + " alpha beta " + logical[3]).c_str());
    expect(activity, 1, 2, "alpha beta");
    expect(activity, 2, 2, "alpha");
    assert(activity.words[1].text == visualSecond);  // Export must not change navigation order.
  }
  const std::vector<std::string> ltr{"alpha", "אחד", "שני", "beta"};
  loadLine(activity, ltr, false);
  expect(activity, 0, 3, "alpha אחד שני beta");
  const std::vector<std::string> firstLine{"אחד", "alpha", "beta", "שני"};
  const std::vector<std::string> secondLine{"واحد", "gamma", "delta", "اثنان"};
  loadLine(activity, firstLine, true);
  loadLine(activity, secondLine, true, 4, 40, 1);
  expect(activity, 0, 7, "אחד alpha beta שני واحد gamma delta اثنان");
  activity.words[4].paragraphStart = true;
  expect(activity, 0, 7, "אחד alpha beta שני\nواحد gamma delta اثنان");
  const std::vector<std::string> plain{"one", "two"};
  loadLine(activity, plain, false);
  activity.words[0].startOffset = activity.words[0].endOffset = UINT32_MAX;
  expect(activity, 0, 1, "one two");
  activity.words[1].startOffset = activity.words[1].endOffset = UINT32_MAX;
  expect(activity, 0, 1, "one two");
  activity.words[0].text = "discre-";
  activity.words[0].startOffset = 0;
  activity.words[0].endOffset = 6;
  activity.words[1].text = "tionary";
  activity.words[1].startOffset = 6;
  activity.words[1].endOffset = 13;
  activity.words[0].discretionaryHyphen = true;
  expect(activity, 0, 1, "discretionary");
  activity.words[0].text = "Café";
  activity.words[0].endOffset = 5;
  activity.words[0].discretionaryHyphen = false;
  activity.words[1].text = "s";
  activity.words[1].startOffset = 5;
  activity.words[1].endOffset = 6;
  expect(activity, 0, 1, "Cafés");
  activity.words[0].text = "Café-";
  activity.words[0].discretionaryHyphen = true;
  expect(activity, 0, 1, "Cafés");
  activity.words[0].endOffset = 6;
  activity.words[0].discretionaryHyphen = false;
  activity.words[1].startOffset = 6;
  activity.words[1].endOffset = 7;
  expect(activity, 0, 1, "Café-s");
  const std::string oversized(CLIPPING_TEXT_MAX + 1, 'x');
  activity.words[0].text = oversized.c_str();
  std::string text;
  assert(!activity.buildSelectedText(0, 0, text));
  std::string expected;
  for (int i = 0; i < 240; ++i) {
    activity.words[i] = {};
    activity.words[i].text = "word";
    activity.words[i].startOffset = (239 - i) * 5;
    activity.words[i].endOffset = activity.words[i].startOffset + 4;
    if (i != 0) expected += ' ';
    expected += "word";
  }
  expect(activity, 0, 239, expected.c_str());
}
"""
with tempfile.TemporaryDirectory() as directory:
    cpp = Path(directory) / "selection.cpp"
    executable = Path(directory) / "selection"
    bidi_object = Path(directory) / "minibidi.o"
    cpp.write_text(harness)
    subprocess.run(["cc", "-c", str(ROOT / "lib/MiniBidi/minibidi.c"), "-o", str(bidi_object)], check=True)
    subprocess.run([
        "c++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
        "-I", str(ROOT / "src"), "-I", str(ROOT / "lib/Memory"),
        "-I", str(ROOT / "test/minibidi_arabic/stubs"),
        "-I", str(ROOT / "lib/MiniBidi"), "-I", str(ROOT / "lib/Utf8"),
        "-I", str(ROOT / "lib/Epub"),
        str(cpp), str(bidi_object), str(ROOT / "lib/MiniBidi/BidiUtils.cpp"),
        str(ROOT / "lib/Utf8/Utf8.cpp"), "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True)
print("Clipping selection text regression checks passed")
