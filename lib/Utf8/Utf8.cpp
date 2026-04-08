#include "Utf8.h"

int utf8CodepointLen(const unsigned char c) {
  if (c < 0x80) return 1;          // 0xxxxxxx
  if ((c >> 5) == 0x6) return 2;   // 110xxxxx
  if ((c >> 4) == 0xE) return 3;   // 1110xxxx
  if ((c >> 3) == 0x1E) return 4;  // 11110xxx
  return 1;                        // fallback for invalid
}

uint32_t utf8NextCodepoint(const unsigned char** string) {
  if (**string == 0) {
    return 0;
  }

  const unsigned char lead = **string;
  const int bytes = utf8CodepointLen(lead);
  const uint8_t* chr = *string;

  // Invalid lead byte (stray continuation byte 0x80-0xBF, or 0xFE/0xFF)
  if (bytes == 1 && lead >= 0x80) {
    (*string)++;
    return REPLACEMENT_GLYPH;
  }

  if (bytes == 1) {
    (*string)++;
    return chr[0];
  }

  // Validate continuation bytes before consuming them
  for (int i = 1; i < bytes; i++) {
    if ((chr[i] & 0xC0) != 0x80) {
      // Missing or invalid continuation byte — skip all bytes consumed so far
      *string += i;
      return REPLACEMENT_GLYPH;
    }
  }

  uint32_t cp = chr[0] & ((1 << (7 - bytes)) - 1);  // mask header bits

  for (int i = 1; i < bytes; i++) {
    cp = (cp << 6) | (chr[i] & 0x3F);
  }

  // Reject overlong encodings, surrogates, and out-of-range values
  const bool overlong = (bytes == 2 && cp < 0x80) || (bytes == 3 && cp < 0x800) || (bytes == 4 && cp < 0x10000);
  const bool surrogate = (cp >= 0xD800 && cp <= 0xDFFF);
  if (overlong || surrogate || cp > 0x10FFFF) {
    (*string)++;
    return REPLACEMENT_GLYPH;
  }

  *string += bytes;

  return cp;
}

int utf8SafeTruncateBuffer(const char* buf, int len) {
  if (len <= 0) return 0;

  // Walk back past continuation bytes (10xxxxxx) to find the lead byte
  int leadPos = len - 1;
  while (leadPos > 0 && (static_cast<uint8_t>(buf[leadPos]) & 0xC0) == 0x80) {
    leadPos--;
  }

  // Determine expected length of the sequence starting at leadPos
  int expectedLen = utf8CodepointLen(static_cast<unsigned char>(buf[leadPos]));
  int actualLen = len - leadPos;

  if (actualLen < expectedLen && leadPos > 0) {
    // Incomplete UTF-8 sequence at the end — exclude it
    return leadPos;
  }
  return len;
}

size_t utf8RemoveLastChar(std::string& str) {
  if (str.empty()) return 0;
  size_t pos = str.size() - 1;
  while (pos > 0 && (static_cast<unsigned char>(str[pos]) & 0xC0) == 0x80) {
    --pos;
  }
  str.resize(pos);
  return pos;
}

// Truncate string by removing N UTF-8 characters from the end
void utf8TruncateChars(std::string& str, const size_t numChars) {
  for (size_t i = 0; i < numChars && !str.empty(); ++i) {
    utf8RemoveLastChar(str);
  }
}


// ---------------------------------------------------------------------------
// Vietnamese NFC normalization
// ---------------------------------------------------------------------------

// Encode a Unicode codepoint as UTF-8 bytes appended to `out`.
static void appendCodepoint(std::string& out, const uint32_t cp) {
  if (cp < 0x80u) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800u) {
    out += static_cast<char>(0xC0u | (cp >> 6));
    out += static_cast<char>(0x80u | (cp & 0x3Fu));
  } else if (cp < 0x10000u) {
    out += static_cast<char>(0xE0u | (cp >> 12));
    out += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
    out += static_cast<char>(0x80u | (cp & 0x3Fu));
  } else {
    out += static_cast<char>(0xF0u | (cp >> 18));
    out += static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu));
    out += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
    out += static_cast<char>(0x80u | (cp & 0x3Fu));
  }
}

// Lookup table: {(base << 16) | mark, composed_result}.
// Covers all Vietnamese precomposed characters U+1EA0-U+1EF9 plus the
// intermediate Latin composites (â ă ê ô ơ ư and uppercase matches).
// Handles both "natural" order (structural modifier before tone) and
// canonical NFD order (dot-below reordered before circumflex/breve).
// Table is sorted by key ascending for binary search.
struct ComposeEntry {
  uint32_t key;     // (base << 16) | mark
  uint32_t result;
};

static const ComposeEntry kComposeTable[] = {
    // Sorted ascending by key = (base << 16) | mark.
    // Uppercase Latin (0x0041–0x0059) come before lowercase (0x0061–0x0079),
    // then accented precomposed bases in codepoint order.

    // ── A  (U+0041) ──────────────────────────────────────────────────────
    {0x00410300u, 0x00C0u},  // A + grave   → À
    {0x00410301u, 0x00C1u},  // A + acute   → Á
    {0x00410302u, 0x00C2u},  // A + ^       → Â
    {0x00410303u, 0x00C3u},  // A + tilde   → Ã
    {0x00410306u, 0x0102u},  // A + breve   → Ă
    {0x00410309u, 0x1EA2u},  // A + hook    → Ả
    {0x00410323u, 0x1EA0u},  // A + dot     → Ạ
    // ── E  (U+0045) ──────────────────────────────────────────────────────
    {0x00450300u, 0x00C8u},  // E + grave   → È
    {0x00450301u, 0x00C9u},  // E + acute   → É
    {0x00450302u, 0x00CAu},  // E + ^       → Ê
    {0x00450303u, 0x1EBCu},  // E + tilde   → Ẽ
    {0x00450309u, 0x1EBAu},  // E + hook    → Ẻ
    {0x00450323u, 0x1EB8u},  // E + dot     → Ẹ
    // ── I  (U+0049) ──────────────────────────────────────────────────────
    {0x00490300u, 0x00CCu},  // I + grave   → Ì
    {0x00490301u, 0x00CDu},  // I + acute   → Í
    {0x00490303u, 0x0128u},  // I + tilde   → Ĩ
    {0x00490309u, 0x1EC8u},  // I + hook    → Ỉ
    {0x00490323u, 0x1ECAu},  // I + dot     → Ị
    // ── O  (U+004F) ──────────────────────────────────────────────────────
    {0x004F0300u, 0x00D2u},  // O + grave   → Ò
    {0x004F0301u, 0x00D3u},  // O + acute   → Ó
    {0x004F0302u, 0x00D4u},  // O + ^       → Ô
    {0x004F0303u, 0x00D5u},  // O + tilde   → Õ
    {0x004F0309u, 0x1ECEu},  // O + hook    → Ỏ
    {0x004F031Bu, 0x01A0u},  // O + horn    → Ơ
    {0x004F0323u, 0x1ECCu},  // O + dot     → Ọ
    // ── U  (U+0055) ──────────────────────────────────────────────────────
    {0x00550300u, 0x00D9u},  // U + grave   → Ù
    {0x00550301u, 0x00DAu},  // U + acute   → Ú
    {0x00550303u, 0x0168u},  // U + tilde   → Ũ
    {0x00550309u, 0x1EE6u},  // U + hook    → Ủ
    {0x0055031Bu, 0x01AFu},  // U + horn    → Ư
    {0x00550323u, 0x1EE4u},  // U + dot     → Ụ
    // ── Y  (U+0059) ──────────────────────────────────────────────────────
    {0x00590300u, 0x1EF2u},  // Y + grave   → Ỳ
    {0x00590301u, 0x00DDu},  // Y + acute   → Ý
    {0x00590303u, 0x1EF8u},  // Y + tilde   → Ỹ
    {0x00590309u, 0x1EF6u},  // Y + hook    → Ỷ
    {0x00590323u, 0x1EF4u},  // Y + dot     → Ỵ
    // ── a  (U+0061) ──────────────────────────────────────────────────────
    {0x00610300u, 0x00E0u},  // a + grave   → à
    {0x00610301u, 0x00E1u},  // a + acute   → á
    {0x00610302u, 0x00E2u},  // a + ^       → â
    {0x00610303u, 0x00E3u},  // a + tilde   → ã
    {0x00610306u, 0x0103u},  // a + breve   → ă
    {0x00610309u, 0x1EA3u},  // a + hook    → ả
    {0x00610323u, 0x1EA1u},  // a + dot     → ạ
    // ── e  (U+0065) ──────────────────────────────────────────────────────
    {0x00650300u, 0x00E8u},  // e + grave   → è
    {0x00650301u, 0x00E9u},  // e + acute   → é
    {0x00650302u, 0x00EAu},  // e + ^       → ê
    {0x00650303u, 0x1EBDu},  // e + tilde   → ẽ
    {0x00650309u, 0x1EBBu},  // e + hook    → ẻ
    {0x00650323u, 0x1EB9u},  // e + dot     → ẹ
    // ── i  (U+0069) ──────────────────────────────────────────────────────
    {0x00690300u, 0x00ECu},  // i + grave   → ì
    {0x00690301u, 0x00EDu},  // i + acute   → í
    {0x00690303u, 0x0129u},  // i + tilde   → ĩ
    {0x00690309u, 0x1EC9u},  // i + hook    → ỉ
    {0x00690323u, 0x1ECBu},  // i + dot     → ị
    // ── o  (U+006F) ──────────────────────────────────────────────────────
    {0x006F0300u, 0x00F2u},  // o + grave   → ò
    {0x006F0301u, 0x00F3u},  // o + acute   → ó
    {0x006F0302u, 0x00F4u},  // o + ^       → ô
    {0x006F0303u, 0x00F5u},  // o + tilde   → õ
    {0x006F0309u, 0x1ECFu},  // o + hook    → ỏ
    {0x006F031Bu, 0x01A1u},  // o + horn    → ơ
    {0x006F0323u, 0x1ECDu},  // o + dot     → ọ
    // ── u  (U+0075) ──────────────────────────────────────────────────────
    {0x00750300u, 0x00F9u},  // u + grave   → ù
    {0x00750301u, 0x00FAu},  // u + acute   → ú
    {0x00750303u, 0x0169u},  // u + tilde   → ũ
    {0x00750309u, 0x1EE7u},  // u + hook    → ủ
    {0x0075031Bu, 0x01B0u},  // u + horn    → ư
    {0x00750323u, 0x1EE5u},  // u + dot     → ụ
    // ── y  (U+0079) ──────────────────────────────────────────────────────
    {0x00790300u, 0x1EF3u},  // y + grave   → ỳ
    {0x00790301u, 0x00FDu},  // y + acute   → ý
    {0x00790303u, 0x1EF9u},  // y + tilde   → ỹ
    {0x00790309u, 0x1EF7u},  // y + hook    → ỷ
    {0x00790323u, 0x1EF5u},  // y + dot     → ỵ
    // ── Â  (U+00C2) ──────────────────────────────────────────────────────
    {0x00C20300u, 0x1EA6u},  // Â + grave   → Ầ
    {0x00C20301u, 0x1EA4u},  // Â + acute   → Ấ
    {0x00C20303u, 0x1EAAu},  // Â + tilde   → Ẫ
    {0x00C20309u, 0x1EA8u},  // Â + hook    → Ẩ
    {0x00C20323u, 0x1EACu},  // Â + dot     → Ậ
    // ── Ê  (U+00CA) ──────────────────────────────────────────────────────
    {0x00CA0300u, 0x1EC0u},  // Ê + grave   → Ề
    {0x00CA0301u, 0x1EBEu},  // Ê + acute   → Ế
    {0x00CA0303u, 0x1EC4u},  // Ê + tilde   → Ễ
    {0x00CA0309u, 0x1EC2u},  // Ê + hook    → Ể
    {0x00CA0323u, 0x1EC6u},  // Ê + dot     → Ệ
    // ── Ô  (U+00D4) ──────────────────────────────────────────────────────
    {0x00D40300u, 0x1ED2u},  // Ô + grave   → Ồ
    {0x00D40301u, 0x1ED0u},  // Ô + acute   → Ố
    {0x00D40303u, 0x1ED6u},  // Ô + tilde   → Ỗ
    {0x00D40309u, 0x1ED4u},  // Ô + hook    → Ổ
    {0x00D40323u, 0x1ED8u},  // Ô + dot     → Ộ
    // ── â  (U+00E2) ──────────────────────────────────────────────────────
    {0x00E20300u, 0x1EA7u},  // â + grave   → ầ
    {0x00E20301u, 0x1EA5u},  // â + acute   → ấ
    {0x00E20303u, 0x1EABu},  // â + tilde   → ẫ
    {0x00E20309u, 0x1EA9u},  // â + hook    → ẩ
    {0x00E20323u, 0x1EADu},  // â + dot     → ậ
    // ── ê  (U+00EA) ──────────────────────────────────────────────────────
    {0x00EA0300u, 0x1EC1u},  // ê + grave   → ề
    {0x00EA0301u, 0x1EBFu},  // ê + acute   → ế
    {0x00EA0303u, 0x1EC5u},  // ê + tilde   → ễ
    {0x00EA0309u, 0x1EC3u},  // ê + hook    → ể
    {0x00EA0323u, 0x1EC7u},  // ê + dot     → ệ
    // ── ô  (U+00F4) ──────────────────────────────────────────────────────
    {0x00F40300u, 0x1ED3u},  // ô + grave   → ồ
    {0x00F40301u, 0x1ED1u},  // ô + acute   → ố
    {0x00F40303u, 0x1ED7u},  // ô + tilde   → ỗ
    {0x00F40309u, 0x1ED5u},  // ô + hook    → ổ
    {0x00F40323u, 0x1ED9u},  // ô + dot     → ộ
    // ── Ă  (U+0102) ──────────────────────────────────────────────────────
    {0x01020300u, 0x1EB0u},  // Ă + grave   → Ằ
    {0x01020301u, 0x1EAEu},  // Ă + acute   → Ắ
    {0x01020303u, 0x1EB4u},  // Ă + tilde   → Ẵ
    {0x01020309u, 0x1EB2u},  // Ă + hook    → Ẳ
    {0x01020323u, 0x1EB6u},  // Ă + dot     → Ặ
    // ── ă  (U+0103) ──────────────────────────────────────────────────────
    {0x01030300u, 0x1EB1u},  // ă + grave   → ằ
    {0x01030301u, 0x1EAFu},  // ă + acute   → ắ
    {0x01030303u, 0x1EB5u},  // ă + tilde   → ẵ
    {0x01030309u, 0x1EB3u},  // ă + hook    → ẳ
    {0x01030323u, 0x1EB7u},  // ă + dot     → ặ
    // ── Ơ  (U+01A0) ──────────────────────────────────────────────────────
    {0x01A00300u, 0x1EDCu},  // Ơ + grave   → Ờ
    {0x01A00301u, 0x1EDAu},  // Ơ + acute   → Ớ
    {0x01A00303u, 0x1EE0u},  // Ơ + tilde   → Ỡ
    {0x01A00309u, 0x1EDEu},  // Ơ + hook    → Ở
    {0x01A00323u, 0x1EE2u},  // Ơ + dot     → Ợ
    // ── ơ  (U+01A1) ──────────────────────────────────────────────────────
    {0x01A10300u, 0x1EDDu},  // ơ + grave   → ờ
    {0x01A10301u, 0x1EDBu},  // ơ + acute   → ớ
    {0x01A10303u, 0x1EE1u},  // ơ + tilde   → ỡ
    {0x01A10309u, 0x1EDFu},  // ơ + hook    → ở
    {0x01A10323u, 0x1EE3u},  // ơ + dot     → ợ
    // ── Ư  (U+01AF) ──────────────────────────────────────────────────────
    {0x01AF0300u, 0x1EEAu},  // Ư + grave   → Ừ
    {0x01AF0301u, 0x1EE8u},  // Ư + acute   → Ứ
    {0x01AF0303u, 0x1EEEu},  // Ư + tilde   → Ữ
    {0x01AF0309u, 0x1EECu},  // Ư + hook    → Ử
    {0x01AF0323u, 0x1EF0u},  // Ư + dot     → Ự
    // ── ư  (U+01B0) ──────────────────────────────────────────────────────
    {0x01B00300u, 0x1EEBu},  // ư + grave   → ừ
    {0x01B00301u, 0x1EE9u},  // ư + acute   → ứ
    {0x01B00303u, 0x1EEFu},  // ư + tilde   → ữ
    {0x01B00309u, 0x1EEDu},  // ư + hook    → ử
    {0x01B00323u, 0x1EF1u},  // ư + dot     → ự
    // ── NFD canonical-order recomposition ────────────────────────────────
    // In NFD, dot-below (CCC 220) sorts BEFORE circumflex/breve (CCC 230).
    // So ậ = a+U+0323+U+0302, ặ = a+U+0323+U+0306, ệ = e+U+0323+U+0302, etc.
    // Greedy first step (a+0323=ạ, e+0323=ẹ, o+0323=ọ) then needs:
    {0x1EA00302u, 0x1EACu},  // Ạ + ^       → Ậ
    {0x1EA00306u, 0x1EB6u},  // Ạ + breve   → Ặ
    {0x1EA10302u, 0x1EADu},  // ạ + ^       → ậ
    {0x1EA10306u, 0x1EB7u},  // ạ + breve   → ặ
    {0x1EB80302u, 0x1EC6u},  // Ẹ + ^       → Ệ
    {0x1EB90302u, 0x1EC7u},  // ẹ + ^       → ệ
    {0x1ECC0302u, 0x1ED8u},  // Ọ + ^       → Ộ
    {0x1ECD0302u, 0x1ED9u},  // ọ + ^       → ộ
};
static constexpr size_t kComposeTableSize = sizeof(kComposeTable) / sizeof(kComposeTable[0]);

// Binary search for (base, mark) composition. Returns 0 if no entry found.
static uint32_t composeOne(const uint32_t base, const uint32_t mark) {
  if (base > 0xFFFFu || mark > 0xFFFFu) return 0u;
  const uint32_t key = (base << 16u) | mark;
  // Binary search
  size_t lo = 0, hi = kComposeTableSize;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (kComposeTable[mid].key == key) return kComposeTable[mid].result;
    if (kComposeTable[mid].key < key)
      lo = mid + 1;
    else
      hi = mid;
  }
  return 0u;
}

std::string utf8NfcNorm(std::string s) {
  if (s.empty()) return s;

  // Fast-path: skip strings with no combining marks (most already-NFC text).
  bool hasCombining = false;
  {
    const auto* p = reinterpret_cast<const unsigned char*>(s.c_str());
    uint32_t cp;
    while ((cp = utf8NextCodepoint(&p))) {
      if (utf8IsVietnameseCombining(cp)) {
        hasCombining = true;
        break;
      }
    }
  }
  if (!hasCombining) return s;

  std::string out;
  out.reserve(s.size());

  const auto* ptr = reinterpret_cast<const unsigned char*>(s.c_str());
  while (*ptr != '\0') {
    uint32_t base = utf8NextCodepoint(&ptr);
    if (base == 0) break;

    // Greedily absorb following Vietnamese combining marks into base.
    while (*ptr != '\0') {
      const auto* markStart = ptr;
      const uint32_t mark = utf8NextCodepoint(&ptr);
      if (mark == 0) break;

      if (!utf8IsVietnameseCombining(mark)) {
        // Non-combining: put it back and stop absorbing.
        ptr = markStart;
        break;
      }

      const uint32_t composed = composeOne(base, mark);
      if (composed != 0u) {
        base = composed;  // absorbed — try next mark
      } else {
        // Cannot compose: emit current base as-is, the unmatched mark becomes
        // the new base token (it will be emitted or further composed next loop).
        appendCodepoint(out, base);
        base = mark;
      }
    }

    appendCodepoint(out, base);
  }

  return out;
}