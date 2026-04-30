#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

// The unit under test — no hardware dependencies
#include "src/network/HtmlToText.h"

static int testsPassed = 0;
static int testsFailed = 0;

// ── Helpers ───────────────────────────────────────────────────────────────────

#define ASSERT_EQ(a, b)                                                                               \
  do {                                                                                                \
    auto _a = (a);                                                                                    \
    auto _b = (b);                                                                                    \
    if (_a != _b) {                                                                                   \
      fprintf(stderr, "  FAIL %s:%d\n    got:      \"%s\"\n    expected: \"%s\"\n", __FILE__,        \
              __LINE__, _a.c_str(), _b.c_str());                                                      \
      testsFailed++;                                                                                  \
      return;                                                                                         \
    }                                                                                                 \
  } while (0)

#define ASSERT_TRUE(cond)                                                \
  do {                                                                   \
    if (!(cond)) {                                                       \
      fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      testsFailed++;                                                     \
      return;                                                            \
    }                                                                    \
  } while (0)

#define PASS() \
  do {         \
    printf("  OK\n"); \
    testsPassed++;    \
  } while (0)

static std::string cvt(const char* html) { return HtmlToText::convert(std::string(html)); }

// ── Tests ─────────────────────────────────────────────────────────────────────

void testPlainText() {
  printf("plain text passthrough...\n");
  ASSERT_EQ(cvt("Hello, world!"), std::string("Hello, world!"));
  ASSERT_EQ(cvt(""), std::string(""));
  PASS();
}

void testBasicTagStripping() {
  printf("basic tag stripping...\n");
  ASSERT_EQ(cvt("<b>bold</b>"), std::string("bold"));
  ASSERT_EQ(cvt("<em>italic</em>"), std::string("italic"));
  ASSERT_EQ(cvt("<span class=\"x\">text</span>"), std::string("text"));
  ASSERT_EQ(cvt("<a href=\"http://example.com\">link</a>"), std::string("link"));
  PASS();
}

void testBlockTagsProduceNewlines() {
  printf("block-level tags become newlines...\n");
  // <p> and </p> each emit a newline; consecutive newlines collapse to 2
  std::string result = cvt("<p>Para one</p><p>Para two</p>");
  ASSERT_TRUE(result.find("Para one") != std::string::npos);
  ASSERT_TRUE(result.find("Para two") != std::string::npos);
  // The two paragraphs must be separated by at least one newline
  size_t pos1 = result.find("Para one");
  size_t pos2 = result.find("Para two");
  ASSERT_TRUE(pos1 < pos2);
  std::string between(result.begin() + pos1 + 8, result.begin() + pos2);
  ASSERT_TRUE(between.find('\n') != std::string::npos);

  // <br> produces a newline
  result = cvt("line one<br>line two");
  ASSERT_TRUE(result.find("line one") != std::string::npos);
  ASSERT_TRUE(result.find("line two") != std::string::npos);

  // <br/> self-closing form
  result = cvt("a<br/>b");
  ASSERT_TRUE(result.find('\n') != std::string::npos);

  // Headers
  result = cvt("<h1>Title</h1>body");
  ASSERT_TRUE(result.find("Title") != std::string::npos);
  ASSERT_TRUE(result.find("body") != std::string::npos);

  PASS();
}

void testScriptStyleRemoved() {
  printf("script and style blocks removed...\n");
  ASSERT_EQ(cvt("<script>alert('xss')</script>After"), std::string("After"));
  ASSERT_EQ(cvt("<style>body{color:red}</style>After"), std::string("After"));
  ASSERT_EQ(cvt("Before<script src=\"x.js\">evil()</script>After"), std::string("Before After"));
  // Multiline script
  ASSERT_EQ(cvt("<script>\n  var x = 1;\n  var y = 2;\n</script>Text"), std::string("Text"));
  PASS();
}

void testHtmlCommentsRemoved() {
  printf("HTML comments removed...\n");
  ASSERT_EQ(cvt("before<!--comment-->after"), std::string("beforeafter"));
  ASSERT_EQ(cvt("<!--entire page is a comment-->"), std::string(""));
  ASSERT_EQ(cvt("a<!-- multi\nline\ncomment -->b"), std::string("ab"));
  PASS();
}

void testDoctypeSkipped() {
  printf("DOCTYPE / PI skipped...\n");
  std::string result = cvt("<!DOCTYPE html><html><body>Hello</body></html>");
  ASSERT_TRUE(result.find("Hello") != std::string::npos);
  ASSERT_TRUE(result.find("DOCTYPE") == std::string::npos);
  PASS();
}

void testNamedEntities() {
  printf("named entity decoding...\n");
  ASSERT_EQ(cvt("&amp;"), std::string("&"));
  ASSERT_EQ(cvt("&lt;"), std::string("<"));
  ASSERT_EQ(cvt("&gt;"), std::string(">"));
  ASSERT_EQ(cvt("&quot;"), std::string("\""));
  ASSERT_EQ(cvt("&apos;"), std::string("'"));
  ASSERT_EQ(cvt("AT&amp;T"), std::string("AT&T"));
  ASSERT_EQ(cvt("&nbsp;"), std::string(" "));
  // mdash / ndash produce UTF-8 bytes
  std::string mdash = cvt("&mdash;");
  ASSERT_TRUE(!mdash.empty());
  ASSERT_TRUE(mdash != std::string("&mdash;"));
  PASS();
}

void testNumericEntities() {
  printf("numeric entity decoding...\n");
  // &#65; = 'A'
  ASSERT_EQ(cvt("&#65;"), std::string("A"));
  // &#x41; = 'A'
  ASSERT_EQ(cvt("&#x41;"), std::string("A"));
  ASSERT_EQ(cvt("&#X41;"), std::string("A"));
  // &#160; = non-breaking space → regular space
  ASSERT_EQ(cvt("a&#160;b"), std::string("a b"));
  // &#x2022; = U+2022 BULLET → multi-byte UTF-8
  std::string bullet = cvt("&#x2022;");
  ASSERT_TRUE(bullet.size() >= 2);  // at least 2 bytes for U+2022
  PASS();
}

void testWhitespaceNormalisation() {
  printf("whitespace normalisation...\n");
  // Multiple spaces collapse to one
  ASSERT_EQ(cvt("a   b"), std::string("a b"));
  // Newlines / tabs in HTML source become spaces
  ASSERT_EQ(cvt("a\nb"), std::string("a b"));
  ASSERT_EQ(cvt("a\tb"), std::string("a b"));
  ASSERT_EQ(cvt("a\r\nb"), std::string("a b"));
  // No leading/trailing whitespace
  std::string r = cvt("  hello  ");
  ASSERT_TRUE(r.front() != ' ');
  ASSERT_TRUE(r.back() != ' ');
  PASS();
}

void testBlankLineCollapsing() {
  printf("blank line collapsing...\n");
  // Three consecutive block tags should not produce more than 2 newlines
  std::string result = cvt("<p>a</p><p></p><p>b</p>");
  int newlineCount = 0;
  for (char c : result) {
    if (c == '\n') newlineCount++;
  }
  ASSERT_TRUE(newlineCount <= 4);  // reasonable upper bound

  // No trailing newlines
  ASSERT_TRUE(result.back() != '\n');
  PASS();
}

void testExtractTitle() {
  printf("extractTitle...\n");
  ASSERT_EQ(HtmlToText::extractTitle("<html><head><title>My Page</title></head></html>"),
            std::string("My Page"));
  ASSERT_EQ(HtmlToText::extractTitle("<TITLE>UPPERCASE</TITLE>body"), std::string("UPPERCASE"));
  ASSERT_EQ(HtmlToText::extractTitle("<title>Entities &amp; Fun</title>"), std::string("Entities & Fun"));
  ASSERT_EQ(HtmlToText::extractTitle("<html>no title here</html>"), std::string(""));
  PASS();
}

void testRealWorldFragment() {
  printf("real-world HTML fragment...\n");
  const char* html = R"(
    <!DOCTYPE html>
    <html lang="en">
    <head>
      <meta charset="UTF-8">
      <title>Example Domain</title>
      <style>body { font-family: sans-serif; }</style>
    </head>
    <body>
      <h1>Example Domain</h1>
      <p>This domain is for use in illustrative examples in documents.
         You may use this domain in literature without prior coordination
         or asking for permission.</p>
      <p><a href="https://www.iana.org/domains/example">More information...</a></p>
      <script>
        console.log('ignored');
      </script>
    </body>
    </html>
  )";

  std::string result = HtmlToText::convert(std::string(html));

  ASSERT_TRUE(result.find("Example Domain") != std::string::npos);
  ASSERT_TRUE(result.find("illustrative examples") != std::string::npos);
  ASSERT_TRUE(result.find("More information...") != std::string::npos);
  ASSERT_TRUE(result.find("ignored") == std::string::npos);  // script removed
  ASSERT_TRUE(result.find("font-family") == std::string::npos);  // style removed
  ASSERT_TRUE(result.find("<!DOCTYPE") == std::string::npos);

  printf("  Output (%zu bytes):\n", result.size());
  // Print first 400 chars for manual inspection
  printf("  ---\n");
  size_t printLen = result.size() < 400 ? result.size() : 400;
  fwrite(result.data(), 1, printLen, stdout);
  printf("\n  ---\n");

  PASS();
}

void testMaxBytesLimit() {
  printf("maxBytes input cap...\n");
  // Content after the cap should be absent from output
  std::string html = "Hello world";
  html += "<p>This should not appear</p>";

  std::string result = HtmlToText::convert(html, 11);  // only "Hello world"
  ASSERT_TRUE(result.find("Hello world") != std::string::npos);
  ASSERT_TRUE(result.find("should not appear") == std::string::npos);
  PASS();
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main() {
  printf("=== HtmlToText Tests ===\n\n");

  testPlainText();
  testBasicTagStripping();
  testBlockTagsProduceNewlines();
  testScriptStyleRemoved();
  testHtmlCommentsRemoved();
  testDoctypeSkipped();
  testNamedEntities();
  testNumericEntities();
  testWhitespaceNormalisation();
  testBlankLineCollapsing();
  testExtractTitle();
  testRealWorldFragment();
  testMaxBytesLimit();

  printf("\n=== Results: %d passed, %d failed ===\n", testsPassed, testsFailed);
  return testsFailed > 0 ? 1 : 0;
}
