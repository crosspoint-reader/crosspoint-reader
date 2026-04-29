#include <cstdio>
#include <string>

#include "../../lib/Epub/Epub/css/CssParser.h"

static int testsPassed = 0;
static int testsFailed = 0;

#define ASSERT_EQ(a, b)                                                         \
  do {                                                                          \
    if ((a) != (b)) {                                                           \
      fprintf(stderr, "  FAIL: %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b); \
      testsFailed++;                                                            \
      return;                                                                   \
    }                                                                           \
  } while (0)

#define ASSERT_TRUE(cond)                                                         \
  do {                                                                            \
    if (!(cond)) {                                                                \
      fprintf(stderr, "  FAIL: %s:%d: %s is false\n", __FILE__, __LINE__, #cond); \
      testsFailed++;                                                              \
      return;                                                                     \
    }                                                                             \
  } while (0)

#define PASS() testsPassed++

void testInlineLineThrough() {
  printf("testInlineLineThrough...\n");
  const CssStyle style = CssParser::parseInlineStyle("text-decoration: line-through");
  ASSERT_TRUE(style.hasTextDecoration());
  ASSERT_EQ(style.textDecoration, CssTextDecoration::LineThrough);
  PASS();
}

void testInlineUnderlineLineThrough() {
  printf("testInlineUnderlineLineThrough...\n");
  const CssStyle style = CssParser::parseInlineStyle("text-decoration: underline line-through");
  ASSERT_TRUE(style.hasTextDecoration());
  ASSERT_EQ(style.textDecoration, CssTextDecoration::UnderlineLineThrough);
  PASS();
}

void testInlineLineThroughUnderlineOrderInsensitive() {
  printf("testInlineLineThroughUnderlineOrderInsensitive...\n");
  const CssStyle style = CssParser::parseInlineStyle("text-decoration: line-through underline");
  ASSERT_TRUE(style.hasTextDecoration());
  ASSERT_EQ(style.textDecoration, CssTextDecoration::UnderlineLineThrough);
  PASS();
}

void testInlineTextDecorationNormalization() {
  printf("testInlineTextDecorationNormalization...\n");
  const CssStyle style = CssParser::parseInlineStyle("TEXT-DECORATION : LINE-THROUGH ;");
  ASSERT_TRUE(style.hasTextDecoration());
  ASSERT_EQ(style.textDecoration, CssTextDecoration::LineThrough);
  PASS();
}

int main() {
  printf("=== EPUB CSS Parser Tests ===\n\n");

  testInlineLineThrough();
  testInlineUnderlineLineThrough();
  testInlineLineThroughUnderlineOrderInsensitive();
  testInlineTextDecorationNormalization();

  printf("\n=== Results: %d passed, %d failed ===\n", testsPassed, testsFailed);
  return testsFailed > 0 ? 1 : 0;
}
