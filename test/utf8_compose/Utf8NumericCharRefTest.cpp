#include <gtest/gtest.h>

#include <string>

#include "Utf8.h"

namespace {

// Convenience wrapper: decode one reference body and return what was appended,
// or "<refused>" when the helper reports failure (so a test cannot confuse
// "decoded to empty" with "rejected").
std::string decodeRef(const std::string& body) {
  std::string out;
  if (!utf8AppendNumericCharRef(body, out)) return "<refused>";
  return out;
}

}  // namespace

// Decimal and hex forms of the same codepoint agree, across UTF-8 widths.
TEST(Utf8NumericCharRef, DecodesDecimalAndHex) {
  EXPECT_EQ(decodeRef("#65"), "A");
  EXPECT_EQ(decodeRef("#201"), "\xC3\x89");              // É U+00C9
  EXPECT_EQ(decodeRef("#xC9"), "\xC3\x89");              // same, hex
  EXPECT_EQ(decodeRef("#xc9"), "\xC3\x89");              // lowercase hex digits
  EXPECT_EQ(decodeRef("#X43"), "C");                     // uppercase 'X' marker
  EXPECT_EQ(decodeRef("#8212"), "\xE2\x80\x94");         // em dash U+2014
  EXPECT_EQ(decodeRef("#x1F4DA"), "\xF0\x9F\x93\x9A");   // 📚 U+1F4DA, 4-byte UTF-8
  EXPECT_EQ(decodeRef("#x10FFFF"), "\xF4\x8F\xBF\xBF");  // last valid scalar
}

// It appends to the output rather than replacing it.
TEST(Utf8NumericCharRef, AppendsToExistingOutput) {
  std::string out = "Zola, ";
  ASSERT_TRUE(utf8AppendNumericCharRef("#201", out));
  EXPECT_EQ(out, "Zola, \xC3\x89");
}

// Bodies that are not a complete, valid numeric reference must be refused
// whole — a partial parse would silently drop characters from a title.
TEST(Utf8NumericCharRef, RefusesMalformedBodies) {
  EXPECT_EQ(decodeRef(""), "<refused>");
  EXPECT_EQ(decodeRef("#"), "<refused>");
  EXPECT_EQ(decodeRef("#x"), "<refused>");
  EXPECT_EQ(decodeRef("amp"), "<refused>");    // named entity, not numeric
  EXPECT_EQ(decodeRef("#12a"), "<refused>");   // hex digit in a decimal body
  EXPECT_EQ(decodeRef("#x12g"), "<refused>");  // non-hex digit
  EXPECT_EQ(decodeRef("# 65"), "<refused>");   // embedded whitespace
}

// Values no UTF-8 string may carry: NUL, UTF-16 surrogates, out of range —
// including digit runs long enough to wrap a 32-bit accumulator back into
// the valid range.
TEST(Utf8NumericCharRef, RefusesInvalidScalars) {
  EXPECT_EQ(decodeRef("#0"), "<refused>");
  EXPECT_EQ(decodeRef("#xD800"), "<refused>");       // first surrogate
  EXPECT_EQ(decodeRef("#xDFFF"), "<refused>");       // last surrogate
  EXPECT_EQ(decodeRef("#x110000"), "<refused>");     // one past U+10FFFF
  EXPECT_EQ(decodeRef("#4294967361"), "<refused>");  // 2^32 + 65: must not wrap to "A"
}

// A refused body must leave the output untouched.
TEST(Utf8NumericCharRef, RefusalAppendsNothing) {
  std::string out = "kept";
  EXPECT_FALSE(utf8AppendNumericCharRef("#xD800", out));
  EXPECT_EQ(out, "kept");
}
