#include <gtest/gtest.h>

#include <algorithm>
#include <string>

#include "VoidElementNormalizer.h"

namespace {

// Runs the whole input through in one chunk.
std::string once(const std::string& in) {
  const size_t cap =
      in.size() + void_elements::MAX_TAG + void_elements::growthBound(in.size() + void_elements::MAX_TAG) + 16;
  std::string out(cap, '\0');
  void_elements::State st;
  const size_t n = void_elements::normalize(in.data(), in.size(), out.data(), out.size(), st, true);
  return out.substr(0, n);
}

// Feeds the input in fixed-size chunks, exercising every split point.
std::string chunked(const std::string& in, const size_t chunk) {
  std::string result;
  void_elements::State st;
  for (size_t off = 0; off < in.size(); off += chunk) {
    const size_t len = std::min(chunk, in.size() - off);
    const bool done = off + len >= in.size();
    const size_t cap = len + void_elements::MAX_TAG + void_elements::growthBound(len + void_elements::MAX_TAG) + 16;
    std::string out(cap, '\0');
    const size_t n = void_elements::normalize(in.data() + off, len, out.data(), out.size(), st, done);
    result += out.substr(0, n);
  }
  return result;
}

// --- the defect this exists for ---------------------------------------------

TEST(VoidElementNormalizer, ClosesTheUnclosedBrThatBrokeRealBooks) {
  EXPECT_EQ(once("<p>a<br>b</p>"), "<p>a<br/>b</p>");
}

TEST(VoidElementNormalizer, ClosesHr) { EXPECT_EQ(once("<div><hr></div>"), "<div><hr/></div>"); }

TEST(VoidElementNormalizer, ClosesEveryVoidElement) {
  EXPECT_EQ(once("<img src=\"x.png\">"), "<img src=\"x.png\"/>");
  EXPECT_EQ(once("<meta charset=\"utf-8\">"), "<meta charset=\"utf-8\"/>");
  EXPECT_EQ(once("<link rel=\"stylesheet\">"), "<link rel=\"stylesheet\"/>");
}

TEST(VoidElementNormalizer, IsCaseInsensitive) { EXPECT_EQ(once("<BR><Hr>"), "<BR/><Hr/>"); }

// --- must change nothing else -----------------------------------------------

TEST(VoidElementNormalizer, LeavesAlreadySelfClosingTagsAlone) {
  EXPECT_EQ(once("<br/>"), "<br/>");
  EXPECT_EQ(once("<br />"), "<br />");
}

TEST(VoidElementNormalizer, LeavesNonVoidElementsAlone) {
  // <p> genuinely needs its closing tag; closing it here would change the tree.
  EXPECT_EQ(once("<p>text</p>"), "<p>text</p>");
  EXPECT_EQ(once("<div><span>x</span></div>"), "<div><span>x</span></div>");
}

TEST(VoidElementNormalizer, LeavesClosingTagsCommentsAndPisAlone) {
  EXPECT_EQ(once("</br>"), "</br>");
  EXPECT_EQ(once("<!-- <br> -->"), "<!-- <br> -->");
  EXPECT_EQ(once("<?xml version=\"1.0\"?>"), "<?xml version=\"1.0\"?>");
  EXPECT_EQ(once("<!DOCTYPE html>"), "<!DOCTYPE html>");
}

TEST(VoidElementNormalizer, DoesNotMatchNamesThatMerelyStartWithAVoidName) {
  // "break" and "hrefish" are not void elements; only an exact name match counts.
  EXPECT_EQ(once("<break>x</break>"), "<break>x</break>");
  EXPECT_EQ(once("<brx>"), "<brx>");
}

TEST(VoidElementNormalizer, PassesPlainTextThrough) {
  EXPECT_EQ(once("no markup at all"), "no markup at all");
  EXPECT_EQ(once(""), "");
}

// --- '>' inside an attribute value -------------------------------------------
//
// Legal XML: only '<' and '&' must be escaped in an attribute value. A naive scan
// for the first '>' splits the tag mid-attribute and writes the "/" inside the
// quoted value, corrupting a document that parsed correctly before.

TEST(VoidElementNormalizer, ClosesAtTheRealTagEndNotAQuotedGt) {
  EXPECT_EQ(once("<img alt=\"a > b\" src=\"x.png\">"), "<img alt=\"a > b\" src=\"x.png\"/>");
}

TEST(VoidElementNormalizer, HandlesSingleQuotedAttributes) {
  EXPECT_EQ(once("<img alt='a > b'/>"), "<img alt='a > b'/>");
  EXPECT_EQ(once("<br title='x > y'>"), "<br title='x > y'/>");
}

TEST(VoidElementNormalizer, LeavesAQuotedGtInANonVoidElementAlone) {
  EXPECT_EQ(once("<p title=\"1 > 0\">hi</p>"), "<p title=\"1 > 0\">hi</p>");
}

TEST(VoidElementNormalizer, QuotedGtSurvivesEveryChunkSplit) {
  const std::string in = "x<img alt=\"a > b\" src=\"y\">z";
  const std::string expected = "x<img alt=\"a > b\" src=\"y\"/>z";
  for (size_t chunk = 1; chunk <= in.size(); ++chunk) {
    EXPECT_EQ(chunked(in, chunk), expected) << "chunk size " << chunk;
  }
}

TEST(VoidElementNormalizer, AQuotedGtInsideACommentChangesNothing) {
  EXPECT_EQ(once("<!-- a > b <br> -->"), "<!-- a > b <br> -->");
}

// --- comments and CDATA are contents, not markup ------------------------------

TEST(VoidElementNormalizer, NeverRewritesInsideAComment) {
  EXPECT_EQ(once("<!-- <br> --><br>"), "<!-- <br> --><br/>");
  EXPECT_EQ(once("<!-- a > b <br> and <hr> -->"), "<!-- a > b <br> and <hr> -->");
}

TEST(VoidElementNormalizer, NeverRewritesInsideCdata) {
  // CDATA content is text the document means to show. Rewriting it would change
  // what the reader displays, not just how it parses.
  EXPECT_EQ(once("<![CDATA[write <br> to break]]>"), "<![CDATA[write <br> to break]]>");
  EXPECT_EQ(once("<![CDATA[div > p { }]]><br>"), "<![CDATA[div > p { }]]><br/>");
}

TEST(VoidElementNormalizer, HandlesTerminatorsThatStraddleChunks) {
  const std::string in = "<!-- <br> --><br><![CDATA[<hr>]]><hr>";
  const std::string expected = "<!-- <br> --><br/><![CDATA[<hr>]]><hr/>";
  for (size_t chunk = 1; chunk <= in.size(); ++chunk) {
    EXPECT_EQ(chunked(in, chunk), expected) << "chunk size " << chunk;
  }
}

TEST(VoidElementNormalizer, HandlesAnExtraDashBeforeCommentClose) {
  EXPECT_EQ(once("<!-- x ---><br>"), "<!-- x ---><br/>");
}

// --- chunking must never change the answer ------------------------------------

TEST(VoidElementNormalizer, EveryChunkSizeAgreesWithASingleChunk) {
  const std::string corpus[] = {
      "<p>a<br>b</p>",
      "<img alt=\"a > b\" src='c>d'><hr/><br>",
      "<!DOCTYPE html><html><body><p>x<br>y</p></body></html>",
      "<!-- <br> --><![CDATA[<br>]]><br>",
      "text with no markup",
      "<BR><Hr   ><img\n  src=\"x\"\n>",
      "<a href=\"#n\">1</a><br><span>t</span>",
  };
  for (const auto& in : corpus) {
    const std::string expected = once(in);
    for (size_t chunk = 1; chunk <= in.size(); ++chunk) {
      EXPECT_EQ(chunked(in, chunk), expected) << "input: " << in << " chunk: " << chunk;
    }
  }
}

TEST(VoidElementNormalizer, OnlyEverInsertsForwardSlashes) {
  // The strongest invariant available cheaply: removing every '/' from the output
  // must give back the input with its own '/' removed. Nothing else may change.
  const std::string corpus[] = {
      "<p>a<br>b</p>", "<img alt=\"a > b\">", "<!-- <br> -->", "<![CDATA[<hr>]]>", "<div><hr></div>",
  };
  auto strip = [](std::string v) {
    v.erase(std::remove(v.begin(), v.end(), '/'), v.end());
    return v;
  };
  for (const auto& in : corpus) {
    EXPECT_EQ(strip(once(in)), strip(in)) << "input: " << in;
  }
}

// --- chunk-boundary behaviour ------------------------------------------------

TEST(VoidElementNormalizer, HandlesATagSplitAcrossChunks) {
  const std::string in = "<p>a<br>b</p>";
  for (size_t chunk = 1; chunk <= in.size(); ++chunk) {
    EXPECT_EQ(chunked(in, chunk), "<p>a<br/>b</p>") << "chunk size " << chunk;
  }
}

TEST(VoidElementNormalizer, HandlesManyVoidTagsSplitAtEveryOffset) {
  const std::string in = "x<br>y<hr>z<img src=\"a\">w";
  const std::string expected = "x<br/>y<hr/>z<img src=\"a\"/>w";
  for (size_t chunk = 1; chunk <= in.size(); ++chunk) {
    EXPECT_EQ(chunked(in, chunk), expected) << "chunk size " << chunk;
  }
}

TEST(VoidElementNormalizer, PassesThroughAnAbsurdlyLongUnterminatedTag) {
  // Longer than MAX_TAG with no '>': emitted verbatim rather than held forever.
  const std::string in = "<" + std::string(void_elements::MAX_TAG + 20, 'a');
  EXPECT_EQ(once(in), in);
}

TEST(VoidElementNormalizer, GrowthBoundCoversTheWorstCase) {
  // Densest possible growth: nothing but "<br>".
  std::string in;
  for (int i = 0; i < 100; ++i) in += "<br>";
  const std::string out = once(in);
  EXPECT_EQ(out.size(), in.size() + 100);
  EXPECT_LE(100u, void_elements::growthBound(in.size()));
}

}  // namespace
