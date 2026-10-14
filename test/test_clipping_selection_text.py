"""Run with python3 test/test_clipping_selection_text.py (requires a host C/C++ compiler)."""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "src/activities/reader/ClipSelectionActivity.cpp").read_text()
reader_source = (ROOT / "src/activities/reader/EpubReaderActivity.cpp").read_text()
header = (ROOT / "src/activities/reader/ClipSelectionActivity.h").read_text()
word_box = "struct WordBox {" + header.split("struct WordBox {", 1)[1].split("  };", 1)[0] + "};"
clean_word = "const char* cleanWordStart" + source.split("const char* cleanWordStart", 1)[1].split(
    "}  // namespace", 1
)[0]
build_text = "bool ClipSelectionActivity::buildSelectedText" + source.split(
    "bool ClipSelectionActivity::buildSelectedText", 1
)[1].split("void ClipSelectionActivity::confirmSelection", 1)[0]
memory_guard = "  const auto heap = HalMemory::getDefaultHeap();" + source.split(
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
#include <Epub/ReaderRenderSpec.h>
namespace BidiUtils { enum class BidiBaseDir : int8_t { AUTO = -1, LTR = 0, RTL = 1 }; }
struct EpdFontFamily { enum Style { REGULAR, UNDERLINE = 4 }; };
constexpr size_t FONT_PREWARM_TEXT_MAX = 2048;
constexpr int TAG_PageLine = 1;
struct Rect { int x = 0, y = 0, width = 0, height = 0; };
enum class Color { LightGray };
struct Block {
  struct Style { bool isRtl = false; int8_t characterSpacing = 0; } style;
  struct Range { uint32_t start = 0, end = 3; } range;
  using SourceRange = Range;
  const char* text = "one";
  bool paragraphStart = true;
  bool prefixSpace = false;
  bool valid() const { return true; }
  const Style& getBlockStyle() const { return style; }
  int getRubyShift(int) const { return 0; }
  uint16_t wordCount() const { return prefixSpace ? 2 : 1; }
  const char* wordText(uint16_t i) const { return prefixSpace && i == 0 ? " " : text; }
  EpdFontFamily::Style wordStyle(uint16_t) const { return EpdFontFamily::REGULAR; }
  int wordXpos(uint16_t) const { return 0; }
  Range wordSourceRange(uint16_t i) const {
    return prefixSpace && i == 0 ? Range{range.start, range.start + 1} : range;
  }
  bool wordStartsParagraph(uint16_t i) const { return paragraphStart && i == 0; }
  bool wordHasDiscretionaryHyphen(uint16_t) const { return false; }
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
  bool isSdCardFont(int) const { return false; }
  int getFontAscenderSize(int) const { return 12; }
  int getLineHeight(int) const { return 16; }
  RenderMode getRenderMode() const { return BW; }
  void getOrientedViewableTRBL(int* top, int* right, int* bottom, int* left) const {
    *top = *right = *bottom = *left = 0;
  }
  int getTextAdvanceX(int, const char* text, EpdFontFamily::Style, int8_t tracking = 0) const {
    const int length = std::char_traits<char>::length(text);
    return length * 8 + (length - 1) * tracking;
  }
  void ensureSdCardFontReady(int, const char*, uint8_t) {}
  void drawText(int font, int x, int, const char* text, bool, EpdFontFamily::Style style,
                BidiUtils::BidiBaseDir = BidiUtils::BidiBaseDir::AUTO, int8_t tracking = 0) const {
    draws.push_back({x, getTextAdvanceX(font, text, style, tracking), tracking});
  }
  void fillRect(int x, int, int width, int, bool) const { fills.push_back({x, width, 0}); }
  void fillRectDither(int x, int, int width, int, Color) const { fills.push_back({x, width, 0}); }
  void drawRect(int, int, int, int, bool) const {}
  void drawPixel(int, int, bool) const {}
};
struct Theme {
  void drawSelectionHandle(const Renderer&, Rect, bool) const {}
  void drawSelectionActions(const Renderer&, Rect) const {}
} GUI;
struct Input { bool hasTouch() const { return false; } };
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
struct ClipSelectionActivity {
""" + word_box + r"""
  std::unique_ptr<WordBox[]> words = makeUniqueNoThrow<WordBox[]>(240);
  static constexpr size_t MAX_SELECTABLE_WORDS = 240;
  Renderer renderer;
  Input mappedInput;
  std::vector<std::unique_ptr<Page>> pages;
  size_t wordCount = 0;
  uint16_t rowCount = 0;
  int fontId = 0, lineHeight = 16, marginLeft = 0, marginTop = 0;
  int selected = 0, rangeStart = -1, currentPageOffset = 0;
  bool touchDragSelecting = false;
  bool buildSelectedText(int first, int last, std::string& text) const;
  bool extractWords();
  void drawWordClean(int, int, int) const;
  void drawWordHighlight(int, int, int, int, int) const;
  void drawSelection() const;
  void prewarmWord(int) const {}
  void clearGapBetween(const WordBox&, const WordBox&, int, int) const {}
  void ditherGapBetween(const WordBox&, const WordBox&, int, int) const {}
  int textOffset() const { return 0; }
  int textXOffset() const { return 0; }
  Rect handleRect(int, bool) const { return {}; }
  Rect actionRect() const { return {}; }
};
""" + build_text + extract_words + clean_draw + highlight_draw + selection_draw + r"""
struct EpubReaderActivity {
  Renderer& renderer;
  explicit EpubReaderActivity(Renderer& renderer) : renderer(renderer) {}
  struct Section { int currentPage = 0, pageCount = 1; } sectionData;
  Section* section = &sectionData;
  int currentSpineIndex = 0, buildViewportWidth = 480, buildViewportHeight = 800;
  int clippingAtPoint(const Page&, int, int) const;
  void drawClippingHighlights(const Page&, int, int, int) const;
};
""" + word_rect + saved_hit + saved_draw + r"""
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
int main() {
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
    activity.drawWordHighlight(1, 1, 1, 0, 0);
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
