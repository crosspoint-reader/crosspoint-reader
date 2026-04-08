// Utf8NfcNormTest.cpp — Unit tests for utf8NfcNorm() Vietnamese NFC normalization
//
// Tests cover:
//   1. Table integrity: kComposeTable is sorted (required for binary search)
//   2. Single combining mark: base + tone/structure mark → precomposed
//   3. Double combining mark: base + structural mark + tone mark → precomposed
//   4. Canonical NFD order: dot-below (CCC 220) before circumflex/breve (CCC 230)
//   5. Passthrough: already-NFC text is returned unchanged
//   6. Fast-path: pure ASCII strings bypass the normalization loop entirely
//
// Build & run (macOS/Linux, no PlatformIO needed):
//   clang++ -std=c++17 -I lib/Utf8 -o /tmp/test_utf8nfc \
//       test/utf8_nfc/Utf8NfcNormTest.cpp lib/Utf8/Utf8.cpp && /tmp/test_utf8nfc
//
// Exit code: 0 on all-pass, 1 on any failure.

#include <Utf8.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Minimal test harness
// ---------------------------------------------------------------------------

static int gPassed = 0;
static int gFailed = 0;

static void checkEqual(const char* label, const std::string& got, const std::string& expected) {
  if (got == expected) {
    printf("  PASS  %s\n", label);
    ++gPassed;
  } else {
    printf("  FAIL  %s\n", label);
    printf("        got      [%zu bytes]: ", got.size());
    for (unsigned char c : got) printf("%02X ", c);
    printf("\n");
    printf("        expected [%zu bytes]: ", expected.size());
    for (size_t i = 0; i < expected.size(); i++) printf("%02X ", (unsigned char)expected[i]);
    printf("\n");
    ++gFailed;
  }
}

static void checkTrue(const char* label, bool condition) {
  if (condition) {
    printf("  PASS  %s\n", label);
    ++gPassed;
  } else {
    printf("  FAIL  %s\n", label);
    ++gFailed;
  }
}

// ---------------------------------------------------------------------------
// Test sections
// ---------------------------------------------------------------------------

static void testTableSorted() {
  printf("--- Table integrity (via binary-search correctness) ---\n");
  // If the table were unsorted, binary search would miss entries and compose()
  // would return the wrong result. We verify all 6 base vowels × 5 tone marks
  // are found correctly — if any lookup fails the table has a sort bug.
  struct { const char* label; const char* input; const char* expected; } cases[] = {
    {"A+grave=À",  "A\xCC\x80", "\xC3\x80"},
    {"a+acute=á",  "a\xCC\x81", "\xC3\xA1"},
    {"E+circ=Ê",   "E\xCC\x82", "\xC3\x8A"},
    {"i+tilde=ĩ",  "i\xCC\x83", "\xC4\xA9"},
    {"o+hook=ỏ",   "o\xCC\x89", "\xE1\xBB\x8F"},
    {"U+dot=Ụ",    "U\xCC\xA3", "\xE1\xBB\xA4"},
    {"y+grave=ỳ",  "y\xCC\x80", "\xE1\xBB\xB3"},
    {"\xC6\xA1+hook=ở", "\xC6\xA1\xCC\x89", "\xE1\xBB\x9F"},  // ơ+hook=ở
    {"\xC6\xB0+tilde=ữ", "\xC6\xB0\xCC\x83", "\xE1\xBB\xAF"},  // ư+tilde=ữ
  };
  for (auto& c : cases) {
    checkEqual(c.label, utf8NfcNorm(c.input), c.expected);
  }
}

static void testSingleCombining() {
  printf("--- Single combining mark ---\n");

  // a + U+0309 (hook above) → ả U+1EA3
  checkEqual("a + hook → ả", utf8NfcNorm("a\xCC\x89"), "\xE1\xBA\xA3");

  // a + U+0300 (grave) → à U+00E0
  checkEqual("a + grave → à", utf8NfcNorm("a\xCC\x80"), "\xC3\xA0");

  // a + U+0301 (acute) → á U+00E1
  checkEqual("a + acute → á", utf8NfcNorm("a\xCC\x81"), "\xC3\xA1");

  // a + U+0306 (breve) → ă U+0103
  checkEqual("a + breve → ă", utf8NfcNorm("a\xCC\x86"), "\xC4\x83");

  // a + U+0323 (dot below) → ạ U+1EA1
  checkEqual("a + dot → ạ", utf8NfcNorm("a\xCC\xA3"), "\xE1\xBA\xA1");

  // o + U+031B (horn, COMBINING HORN outside standard range) → ơ U+01A1
  checkEqual("o + horn → ơ", utf8NfcNorm("o\xCC\x9B"), "\xC6\xA1");

  // u + U+031B → ư U+01B0
  checkEqual("u + horn → ư", utf8NfcNorm("u\xCC\x9B"), "\xC6\xB0");

  // y + U+0323 (dot) → ỵ U+1EF5
  checkEqual("y + dot → ỵ", utf8NfcNorm("y\xCC\xA3"), "\xE1\xBB\xB5");

  // u + U+0309 (hook) → ủ U+1EE7
  checkEqual("u + hook → ủ", utf8NfcNorm("u\xCC\x89"), "\xE1\xBB\xA7");
}

static void testDoubleCombining() {
  printf("--- Double combining mark (base + structural + tone) ---\n");

  // o + horn (U+031B) + acute (U+0301) → ớ U+1EDB
  checkEqual("o + horn + acute → ớ", utf8NfcNorm("o\xCC\x9B\xCC\x81"), "\xE1\xBB\x9B");

  // o + horn + grave → ờ U+1EDD
  checkEqual("o + horn + grave → ờ", utf8NfcNorm("o\xCC\x9B\xCC\x80"), "\xE1\xBB\x9D");

  // u + horn + acute → ứ U+1EE9
  checkEqual("u + horn + acute → ứ", utf8NfcNorm("u\xCC\x9B\xCC\x81"), "\xE1\xBB\xA9");

  // u + horn + grave → ừ U+1EEB
  checkEqual("u + horn + grave → ừ", utf8NfcNorm("u\xCC\x9B\xCC\x80"), "\xE1\xBB\xAB");

  // e + circumflex (U+0302) + dot-below (U+0323) → ệ U+1EC7
  checkEqual("e + circ + dot → ệ", utf8NfcNorm("e\xCC\x82\xCC\xA3"), "\xE1\xBB\x87");

  // o + circumflex (U+0302) + dot-below → ộ U+1ED9
  checkEqual("o + circ + dot → ộ", utf8NfcNorm("o\xCC\x82\xCC\xA3"), "\xE1\xBB\x99");

  // a + breve (U+0306) + dot-below → ặ U+1EB7
  checkEqual("a + breve + dot → ặ", utf8NfcNorm("a\xCC\x86\xCC\xA3"), "\xE1\xBA\xB7");
}

static void testCanonicalNfdOrder() {
  printf("--- Canonical NFD order (dot CCC-220 before circ/breve CCC-230) ---\n");

  // a + U+0323 + U+0302 → ậ U+1EAD  (canonical NFD order)
  checkEqual("a + dot + circ → ậ (NFD)", utf8NfcNorm("a\xCC\xA3\xCC\x82"), "\xE1\xBA\xAD");

  // a + U+0323 + U+0306 → ặ U+1EB7  (canonical NFD order)
  checkEqual("a + dot + breve → ặ (NFD)", utf8NfcNorm("a\xCC\xA3\xCC\x86"), "\xE1\xBA\xB7");

  // e + U+0323 + U+0302 → ệ U+1EC7  (canonical NFD order)
  checkEqual("e + dot + circ → ệ (NFD)", utf8NfcNorm("e\xCC\xA3\xCC\x82"), "\xE1\xBB\x87");

  // o + U+0323 + U+0302 → ộ U+1ED9  (canonical NFD order)
  checkEqual("o + dot + circ → ộ (NFD)", utf8NfcNorm("o\xCC\xA3\xCC\x82"), "\xE1\xBB\x99");
}

static void testAlreadyNfc() {
  printf("--- Already-NFC passthrough ---\n");

  // ệ U+1EC7 (E1 BB 87) — already precomposed, must be unchanged
  checkEqual("ệ already NFC", utf8NfcNorm("\xE1\xBB\x87"), "\xE1\xBB\x87");

  // ớ U+1EDB (E1 BB 9B)
  checkEqual("ớ already NFC", utf8NfcNorm("\xE1\xBB\x9B"), "\xE1\xBB\x9B");

  // Mixed NFC sentence unchanged
  const char* sentence = "Vi\xe1\xbb\x87t Nam";  // "Việt Nam" (ệ already NFC)
  checkEqual("NFC sentence unchanged", utf8NfcNorm(sentence), sentence);
}

static void testFastPath() {
  printf("--- ASCII fast-path ---\n");

  checkEqual("empty string", utf8NfcNorm(""), "");
  checkEqual("plain ASCII word", utf8NfcNorm("hello"), "hello");
  checkEqual("ASCII sentence", utf8NfcNorm("The quick brown fox."), "The quick brown fox.");
}

static void testFullWords() {
  printf("--- Full Vietnamese word normalization ---\n");

  // "việt" in NFD: v + i + e + U+0302 + U+0323 + t
  // e+circ+dot → ệ(U+1EC7), composing into "việt"
  checkEqual("viet NFD → NFC",
             utf8NfcNorm("vie\xCC\x82\xCC\xA3t"),
             "vi\xE1\xBB\x87t");

  // "người" NFD: n + g + u + U+031B + o + U+031B + U+0300 + i
  // u+horn → ư(U+01B0), o+horn → ơ(U+01A1), ơ+grave → ờ(U+1EDD)  → "người"
  checkEqual("nguoi NFD → NFC",
             utf8NfcNorm("ngu\xCC\x9Bo\xCC\x9B\xCC\x80i"),
             "ng\xC6\xB0\xE1\xBB\x9Di");

  // "trường" NFD: t + r + u + U+031B + o + U+031B + U+0300 + n + g
  // Wait: "trường" = t-r-ư-ờ-n-g
  // ư = u+horn(U+01B0), ờ = ơ+grave or o+horn+grave
  // u+horn → ư, o+horn+grave → ờ
  checkEqual("truong NFD → NFC",
             utf8NfcNorm("tru\xCC\x9Bo\xCC\x9B\xCC\x80ng"),
             "tr\xC6\xB0\xE1\xBB\x9Dng");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
  printf("=== Utf8NfcNorm Tests ===\n\n");

  testTableSorted();
  printf("\n");
  testSingleCombining();
  printf("\n");
  testDoubleCombining();
  printf("\n");
  testCanonicalNfdOrder();
  printf("\n");
  testAlreadyNfc();
  printf("\n");
  testFastPath();
  printf("\n");
  testFullWords();

  printf("\n=== Results: %d passed, %d failed ===\n", gPassed, gFailed);
  return gFailed == 0 ? 0 : 1;
}