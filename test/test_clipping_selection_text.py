"""Run with python3 test/test_clipping_selection_text.py (requires a host C/C++ compiler)."""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "src/activities/reader/ClipSelectionActivity.cpp").read_text()
reader_source = (ROOT / "src/activities/reader/EpubReaderActivity.cpp").read_text()
header = (ROOT / "src/activities/reader/ClipSelectionActivity.h").read_text()
word_box = "struct WordBox {" + header.split("struct WordBox {", 1)[1].split("  };", 1)[0] + "};"
action_helpers = "\n".join(line for line in header.splitlines()
                           if "int firstAction() const" in line or "int actionCount() const" in line)
clean_word = "const char* cleanWordStart" + source.split("const char* cleanWordStart", 1)[1].split(
    "}  // namespace", 1
)[0]
build_text = "bool ClipSelectionActivity::buildSelectedText" + source.split(
    "bool ClipSelectionActivity::buildSelectedText", 1
)[1].split("void ClipSelectionActivity::confirmSelection", 1)[0]
confirm_source = source.split("void ClipSelectionActivity::confirmSelection", 1)[1]
memory_guard = "  const auto heap = HalMemory::getDefaultHeap();" + confirm_source.split(
    "  const auto heap = HalMemory::getDefaultHeap();", 1
)[1].split("  if (lowMemory ||", 1)[0]

def method(name, next_name, text=source):
    signature = text.index(name)
    start = text.rfind("\n", 0, signature) + 1
    return text[start:text.index(next_name, signature)].rstrip()

extract_words = method("bool ClipSelectionActivity::extractWords", "int ClipSelectionActivity::closestInRow")
gap_draw = method("void ClipSelectionActivity::ditherGapBetween", "bool ClipSelectionActivity::renderIncremental")
incremental_draw = method("bool ClipSelectionActivity::renderIncremental", "void ClipSelectionActivity::drawSelection")
selection_draw = method("void ClipSelectionActivity::drawSelection", "void ClipSelectionActivity::render(")
word_font = method("int clippingWordFont", "Rect clippingWordRect", reader_source)
word_rect = method("Rect clippingWordRect", "int clampPercent", reader_source)
saved_hit = method("int EpubReaderActivity::clippingAtPoint", "void EpubReaderActivity::drawClippingHighlights", reader_source)
saved_draw = method("void EpubReaderActivity::drawClippingHighlights", "void EpubReaderActivity::renderStatusBar", reader_source)

# Compile production extraction, export, and redraws with bounded UI/rendering stubs.
harness = r"""
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <climits>
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
#include <Epub/ReaderRenderSpec.h>
namespace BidiUtils { enum class BidiBaseDir : int8_t { AUTO = -1, LTR = 0, RTL = 1 }; }
struct EpdFontFamily { enum Style { REGULAR, UNDERLINE = 4, STRIKETHROUGH = 8 }; };
constexpr int TAG_PageLine = 1, TAG_PageImage = 2;
struct Rect { int x = 0, y = 0, width = 0, height = 0; };
enum class Color { LightGray };
struct Renderer;
struct Block {
  struct Style { bool isRtl = false; int8_t characterSpacing = 0; } style;
  struct Range { uint32_t start = 0, end = 3; } range;
  using SourceRange = Range;
  const char* text = "one";
  std::string packedText;
  std::vector<EpdFontFamily::Style> styles;
  std::vector<int> positions;
  void render(const Renderer&, int, int, int) const;
  bool paragraphStart = true;
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
  int wordXpos(uint16_t i) const { return positions.empty() ? 0 : positions[i]; }
  Range wordSourceRange(uint16_t i) const {
    return prefixSpace && i == 0 ? Range{range.start, range.start + 1} : range;
  }
  bool wordStartsParagraph(uint16_t i) const { return paragraphStart && i == 0; }
  bool wordHasDiscretionaryHyphen(uint16_t) const { return false; }
};
struct PageLine {
  int tag = TAG_PageLine;
  int xPos = 0, yPos = 0;
  Block block;
  int getTag() const { return tag; }
  const Block* getBlock() const { return &block; }
};
struct Page {
  std::vector<std::unique_ptr<PageLine>> elements;
  mutable int renderCalls = 0;
  bool hasImages() const {
    return std::any_of(elements.begin(), elements.end(), [](const auto& e) { return e->getTag() == TAG_PageImage; });
  }
  void render(Renderer&, int, int, int) const;
};
struct FontCache {
  bool scanning = false;
  struct Scope {
    FontCache& cache;
    explicit Scope(FontCache& c) : cache(c) { cache.scanning = true; }
    void endScanAndPrewarm() { cache.scanning = false; }
    ~Scope() { cache.scanning = false; }
  };
  Scope createPrewarmScope() { return Scope(*this); }
};
struct Renderer {
  enum RenderMode { BW };
  enum class DitherMode { Replace, PreserveInk };
  struct Draw { int x, width, tracking; };
  mutable std::vector<Draw> draws;
  mutable std::vector<Draw> fills;
  mutable std::set<std::pair<int, int>> ink;
  mutable std::array<int, 4> clip{0, 0, 1000, 1000};
  mutable FontCache cache;
  int displays = 0;
  FontCache* getFontCacheManager() const { return &cache; }
  void displayBuffer(int) { ++displays; }
  auto getClipRect() const { return std::array<int, 4>{clip[0], clip[1], clip[2] - clip[0], clip[3] - clip[1]}; }
  void setClipRect(int x, int y, int w, int h) const { clip = {x, y, x + w, y + h}; }
  void clearInk(int x, int y, int width, int height) const {
    for (auto it = ink.begin(); it != ink.end();) {
      const auto [px, py] = *it;
      if (px >= std::max(x, clip[0]) && px < std::min(x + width, clip[2]) &&
          py >= std::max(y, clip[1]) && py < std::min(y + height, clip[3])) it = ink.erase(it);
      else ++it;
    }
  }
  void drawLine(int x, int y, int endX, int, int thickness, bool) const {
    for (int py = y; py < y + thickness; ++py)
      for (int px = x; px <= endX; ++px)
        if (px >= clip[0] && px < clip[2] && py >= clip[1] && py < clip[3]) ink.emplace(px, py);
  }
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
  void drawText(int font, int x, int y, const char* text, bool, EpdFontFamily::Style style,
                BidiUtils::BidiBaseDir = BidiUtils::BidiBaseDir::AUTO, int8_t tracking = 0) const {
    if (!clippingText::hasVisibleText(text)) return;
    draws.push_back({x, getTextAdvanceX(font, text, style, tracking), tracking});
    for (int i = 0; text[i]; ++i)
      for (int py = y + 2; py < y + 10; ++py) drawPixel(x + i * (8 + tracking) + 1, py, true);
  }
  void fillRect(int x, int y, int width, int height, bool) const {
    fills.push_back({x, width, 0}); clearInk(x, y, width, height);
  }
  void fillRectDither(int x, int y, int width, int height, Color,
                      DitherMode mode = DitherMode::Replace) const {
    fills.push_back({x, width, 0});
    if (mode == DitherMode::Replace) clearInk(x, y, width, height);
    for (int py = y; py < y + height; ++py)
      for (int px = x; px < x + width; ++px)
        if (px % 2 == 0 && py % 2 == 0 && px >= clip[0] && px < clip[2] && py >= clip[1] && py < clip[3])
          ink.emplace(px, py);
  }
  void drawRect(int x, int y, int w, int h, bool black) const {
    for (int px = x; px < x + w; ++px) { drawPixel(px, y, black); drawPixel(px, y + h - 1, black); }
    for (int py = y; py < y + h; ++py) { drawPixel(x, py, black); drawPixel(x + w - 1, py, black); }
  }
  void drawPixel(int x, int y, bool black) const {
    if (x < clip[0] || x >= clip[2] || y < clip[1] || y >= clip[3]) return;
    if (black) ink.emplace(x,y); else ink.erase({x,y});
  }
};
void Block::render(const Renderer& renderer, int font, int x, int y) const {
  if (renderer.cache.scanning) return;
  // The source line owns decorations, including continuous ink across word gaps.
  for (uint16_t i = 0; i < wordCount(); ++i) {
    renderer.drawText(font, x + wordXpos(i), y, wordText(i), true, wordStyle(i),
                       BidiUtils::BidiBaseDir::AUTO, style.characterSpacing);
    const int right = x + wordXpos(i) + renderer.getTextAdvanceX(font, wordText(i), wordStyle(i), style.characterSpacing);
    for (const auto decoration : {EpdFontFamily::UNDERLINE, EpdFontFamily::STRIKETHROUGH}) {
      if (!(wordStyle(i) & decoration)) continue;
      const int left = i > 0 && (wordStyle(i - 1) & decoration) ? x + wordXpos(i - 1) : x + wordXpos(i);
      const int lineY = y + (decoration == EpdFontFamily::UNDERLINE ? 14 : 8);
      renderer.drawLine(left, lineY, right, lineY, 2, true);
    }
  }
}
void Page::render(Renderer& renderer, int font, int x, int y) const {
  ++renderCalls;
  for (const auto& line : elements)
    if (line->getTag() == TAG_PageLine) line->block.render(renderer, font, x + line->xPos, y + line->yPos);
}
struct HalDisplay { static constexpr int FAST_REFRESH = 0; };
enum { STR_BACK, STR_SELECT, STR_LOOKUP, STR_DONE, STR_DIR_LEFT, STR_DIR_RIGHT };
#define tr(x) x
struct Theme {
  static Theme& getInstance() { static Theme theme; return theme; }
  Rect getScreenSafeArea(const Renderer&, bool, bool) const { return {0,0,1000,1000}; }
  int hints = 0;
  int confirmLabel = -1;
  void drawButtonHints(Renderer&, int, int confirm, int, int) { ++hints; confirmLabel = confirm; }
  void drawSelectionHandle(const Renderer&, Rect, bool) const {}
  void drawSelectionActions(const Renderer&, Rect, int = 3, int = 0) const {}
} GUI;
using UITheme = Theme;
struct Input {
  bool hasTouch() const { return false; }
  struct Labels { int btn1,btn2,btn3,btn4; };
  Labels mapLabels(int a,int b,int c,int d) const { return {a,b,c,d}; }
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
struct ClipSelectionActivity {
""" + word_box + action_helpers + r"""
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
  int selected = 0, rangeStart = -1, currentPageOffset = 0;
  bool touchDragSelecting = false;
  bool isLookup() const { return mode != Mode::Clip; }
  struct Popup { bool isActive() const { return false; } } actionPopup;
  int lastRenderedPageOffset = 0, lastRenderedSelected = 0, lastRenderedRangeStart = -1;
  int lastRenderedTextOffset = 0, lastRenderedTextXOffset = 0;
  bool buildSelectedText(int first, int last, std::string& text) const;
  bool extractWords();
  void drawSelection() const;
  bool renderIncremental();
  void ditherGapBetween(const WordBox&, const WordBox&, int, int) const;
  int testOffsetX = 0, testOffsetY = 0;
  int textOffset() const { return testOffsetY; }
  int textXOffset() const { return testOffsetX; }
  Rect handleRect(int, bool) const { return {}; }
  Rect actionRect() const { return {}; }
};
""" + build_text + extract_words + gap_draw + incremental_draw + selection_draw + r"""
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
int main() {
  for (bool images : {false, true}) for (auto decoration : {EpdFontFamily::UNDERLINE, EpdFontFamily::STRIKETHROUGH})
    for (auto mode : {ClipSelectionActivity::Mode::Clip, ClipSelectionActivity::Mode::Lookup}) {
    ClipSelectionActivity pick;
    pick.mode = mode;
    pick.marginLeft = 11; pick.marginTop = 7;
    pick.testOffsetX = pick.lastRenderedTextXOffset = 7;
    pick.testOffsetY = pick.lastRenderedTextOffset = 11;
    auto page = std::make_unique<Page>();
    for (int row = 0; row < 2; ++row) {
      auto line = std::make_unique<PageLine>();
      line->xPos = 3; line->yPos = row * 24;
      line->block.packedText = row ? std::string("one\0two\0tri\0",12) : std::string("head\0",5);
      line->block.positions = row ? std::vector<int>{0,40,80} : std::vector<int>{0};
      line->block.styles = row ? std::vector<EpdFontFamily::Style>(3,decoration) : std::vector<EpdFontFamily::Style>{EpdFontFamily::REGULAR};
      page->elements.push_back(std::move(line));
    }
    if (images) { auto image = std::make_unique<PageLine>(); image->tag = TAG_PageImage; page->elements.push_back(std::move(image)); }
    pick.pages.push_back(std::move(page));
    assert(pick.extractWords() && pick.wordCount == 4);
    auto cleanPaint = [&] {
      pick.renderer.ink.clear();
      pick.pageAt(0).render(pick.renderer,0,pick.marginLeft+pick.testOffsetX,pick.marginTop+pick.testOffsetY);
      pick.drawSelection();
      pick.lastRenderedSelected = pick.selected;
      pick.lastRenderedRangeStart = pick.rangeStart;
    };
    pick.selected = 1; // First word on a non-first source line.
    cleanPaint();
    auto before = pick.renderer.ink;
    pick.rangeStart = pick.selected;
    pick.pages[0]->renderCalls = GUI.hints = pick.renderer.displays = 0;
    assert(pick.renderIncremental());
    assert(pick.pages[0]->renderCalls == 0 && GUI.hints == 1 && pick.renderer.displays == 1);
    assert(GUI.confirmLabel == (pick.isLookup() ? STR_LOOKUP : STR_DONE));
    assert(pick.renderer.ink == before);
    for (int next : {2,3,2,1,0,3}) {
      pick.selected = next;
      pick.pages[0]->renderCalls = 0;
      const auto beforeMove = pick.renderer.ink;
      const auto clip = pick.renderer.getClipRect();
      bool incremental = pick.renderIncremental();
      assert(incremental == !images);
      assert(pick.renderer.getClipRect() == clip);
      if (images) {
        assert(pick.pages[0]->renderCalls == 0 && pick.renderer.ink == beforeMove);
        cleanPaint();
      }
      else assert(pick.pages[0]->renderCalls == 2);
      auto actual = pick.renderer.ink;
      cleanPaint();
      assert(pick.renderer.ink == actual);
    }
  }
  for (auto decoration : {EpdFontFamily::UNDERLINE, EpdFontFamily::STRIKETHROUGH}) {
    ClipSelectionActivity decorated;
    decorated.marginLeft = 11;
    decorated.marginTop = 7;
    decorated.testOffsetX = 7;
    decorated.testOffsetY = 11;
    auto page = std::make_unique<Page>();
    auto blank = std::make_unique<PageLine>();
    blank->block.text = " ";
    page->elements.push_back(std::move(blank));
    auto line = std::make_unique<PageLine>();
    line->xPos = 3;
    line->yPos = 5;
    line->block.packedText = std::string("one\0two\0", 8);
    line->block.positions = {0, 40};
    line->block.styles = {decoration, decoration};
    line->block.render(decorated.renderer, 0, decorated.marginLeft + line->xPos + decorated.testOffsetX,
                        decorated.marginTop + line->yPos + decorated.testOffsetY);
    page->elements.push_back(std::move(line));
    decorated.pages.push_back(std::move(page));
    assert(decorated.extractWords());
    const auto originalInk = decorated.renderer.ink;
    decorated.rangeStart = 0;
    decorated.selected = 1;
    decorated.drawSelection();
    auto expectedInk = originalInk;
    const int x = decorated.words[0].x + decorated.testOffsetX;
    const int y = decorated.words[0].y + decorated.testOffsetY;
    const int right = decorated.words[1].x + decorated.testOffsetX + decorated.words[1].width;
    for (int py = y; py < y + decorated.words[0].height; ++py)
      for (int px = x; px < right; ++px)
        if (px % 2 == 0 && py % 2 == 0) expectedInk.emplace(px, py);
    const auto& cursor = decorated.words[1];
    const int cx = cursor.x + decorated.testOffsetX, cy = cursor.y + decorated.testOffsetY;
    for (int px = cx; px < cx + cursor.width; ++px) { expectedInk.emplace(px,cy); expectedInk.emplace(px,cy+cursor.height-1); }
    for (int py = cy; py < cy + cursor.height; ++py) { expectedInk.emplace(cx,py); expectedInk.emplace(cx+cursor.width-1,py); }
    assert(expectedInk.size() > originalInk.size());
    assert(decorated.renderer.ink == expectedInk);
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
    assert(activity.renderer.draws.empty());
    assert(activity.renderer.fills.size() == 2);
    for (size_t i = 0; i < 2; ++i) {
      assert(activity.renderer.fills[i].width == activity.words[i].width);
      assert(activity.renderer.fills[i].x == activity.words[i].x);
    }
    for (size_t i = 0; i < 2; ++i) {
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
