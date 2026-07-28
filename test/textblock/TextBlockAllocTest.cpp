// Allocation accounting for the TextBlock render hot path.
//
// TextBlock::render() runs once per line, per page render. On a device with no
// heap compaction, transient allocations here are the ones most likely to
// fragment DRAM -- they are taken and released on every redraw, so their cost
// is churn rather than residency.
//
// These tests replace operator new/delete with counting versions and assert on
// the exact allocation count of a render call. That turns "this change removes
// allocations" from a claim into a CI-enforced invariant.
//
// The counter is only armed around the call under test, so gtest's own
// allocations are not included.

#include <GfxRenderer.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "Epub/blocks/TextBlock.h"
#include "TestHelpers.h"

namespace {

size_t g_allocCount = 0;
bool g_counting = false;

// Arm the counter for one scope.
struct CountScope {
  CountScope() {
    g_allocCount = 0;
    g_counting = true;
  }
  ~CountScope() { g_counting = false; }
  static size_t count() { return g_allocCount; }
};

// A representative justified line: 12 words, no ruby, no focus annotations.
std::unique_ptr<TextBlock> makeTypicalLine(const std::vector<std::string>& ruby = {}) {
  std::vector<std::string> words;
  std::vector<int16_t> xpos;
  std::vector<Style> styles;
  for (int i = 0; i < 12; i++) {
    words.emplace_back("word");
    xpos.push_back(static_cast<int16_t>(i * 55));
    styles.push_back(EpdFontFamily::REGULAR);
  }
  return makeBlock(words, xpos, styles, ruby);
}

}  // namespace

// Counting allocator. Only global replacement is portable enough to catch
// allocations made inside TextBlock.cpp without recompiling it specially.
void* operator new(const size_t n) {
  if (g_counting) g_allocCount++;
  void* p = std::malloc(n == 0 ? 1 : n);
  if (!p) throw std::bad_alloc();
  return p;
}
void* operator new[](const size_t n) { return ::operator new(n); }
void* operator new(const size_t n, const std::nothrow_t&) noexcept {
  if (g_counting) g_allocCount++;
  return std::malloc(n == 0 ? 1 : n);
}
void* operator new[](const size_t n, const std::nothrow_t& tag) noexcept { return ::operator new(n, tag); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }
void operator delete[](void* p, size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

namespace {

// The headline invariant. A ruby-less line is every line of every non-CJK book,
// so rendering one must not touch the heap at all.
TEST(TextBlockAlloc, RubylessRenderAllocatesNothing) {
  auto block = makeTypicalLine();
  ASSERT_FALSE(block->hasRuby());

  const GfxRenderer renderer;
  renderer.recording = false;  // measure production allocations only

  size_t allocs;
  {
    const CountScope scope;
    block->render(renderer, 0, 0, 100);
    allocs = CountScope::count();
  }
  EXPECT_EQ(allocs, 0u) << "ruby-less line render must not allocate";
}

// A page holds roughly this many lines; this is the per-page-render churn.
TEST(TextBlockAlloc, RubylessPageRenderAllocatesNothing) {
  std::vector<std::unique_ptr<TextBlock>> lines;
  for (int i = 0; i < 28; i++) lines.push_back(makeTypicalLine());

  const GfxRenderer renderer;
  renderer.recording = false;

  size_t allocs;
  {
    const CountScope scope;
    for (const auto& line : lines) line->render(renderer, 0, 0, 100);
    allocs = CountScope::count();
  }
  EXPECT_EQ(allocs, 0u) << "28-line page render must not allocate";
}

// Ruby lines legitimately allocate scratch arrays -- the point of the change is
// that only they do. Pin that the ruby path still runs rather than being
// disabled, and report its cost.
TEST(TextBlockAlloc, RubyRenderStillAllocatesAndDraws) {
  std::vector<std::string> ruby(12, "");
  ruby[0] = "kana";
  auto block = makeTypicalLine(ruby);
  ASSERT_TRUE(block->hasRuby());

  const GfxRenderer counting;
  counting.recording = false;
  size_t allocs;
  {
    const CountScope scope;
    block->render(counting, 0, 0, 100);
    allocs = CountScope::count();
  }
  EXPECT_GT(allocs, 0u) << "ruby path should still allocate its scratch arrays";

  const GfxRenderer recording;
  block->render(recording, 0, 0, 100);
  int supCount = 0;
  for (const auto& call : recording.textCalls) {
    if ((call.style & EpdFontFamily::SUP) != 0) supCount++;
  }
  EXPECT_EQ(supCount, 1) << "ruby annotation must still be drawn";
}

}  // namespace
