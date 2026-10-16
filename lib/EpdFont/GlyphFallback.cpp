#include "GlyphFallback.h"

#include <cstddef>

namespace {

struct Fallback {
  uint16_t from;
  uint16_t to;
};

// Sorted by `from` for the binary search below.
//
// IPA Extensions (U+0250-U+02AF) and the Spacing Modifier Letters a
// transcription uses (U+02B0-U+02D1). Shape first, then the conventional ASCII
// spelling used by X-SAMPA where shape gives no answer.
constexpr Fallback FALLBACKS[] = {
    {0x0250, 'a'},   // ɐ turned a
    {0x0251, 'a'},   // ɑ script a
    {0x0252, 'a'},   // ɒ turned script a
    {0x0253, 'b'},   // ɓ b with hook
    {0x0254, 'o'},   // ɔ open o
    {0x0256, 'd'},   // ɖ d with tail
    {0x0257, 'd'},   // ɗ d with hook
    {0x0259, 'e'},   // ə schwa -- a turned e
    {0x025A, 'e'},   // ɚ schwa with hook
    {0x025B, 'e'},   // ɛ open e
    {0x025C, 'e'},   // ɜ reversed open e
    {0x025E, 'e'},   // ɞ closed reversed open e
    {0x025F, 'j'},   // ɟ dotless j with stroke
    {0x0260, 'g'},   // ɠ g with hook
    {0x0261, 'g'},   // ɡ script g -- this IS a g
    {0x0263, 'y'},   // ɣ gamma
    {0x0265, 'y'},   // ɥ turned h
    {0x0266, 'h'},   // ɦ h with hook
    {0x0268, 'i'},   // ɨ i with stroke
    {0x026A, 'I'},   // ɪ small capital I
    {0x026B, 'l'},   // ɫ l with middle tilde
    {0x026C, 'l'},   // ɬ l with belt
    {0x026D, 'l'},   // ɭ l with retroflex hook
    {0x026F, 'w'},   // ɯ turned m
    {0x0270, 'w'},   // ɰ turned m with long leg
    {0x0271, 'm'},   // ɱ m with hook
    {0x0272, 'n'},   // ɲ n with left hook
    {0x0273, 'n'},   // ɳ n with retroflex hook
    {0x0274, 'N'},   // ɴ small capital N
    {0x0275, 'o'},   // ɵ barred o
    {0x0277, 'w'},   // ɷ closed omega
    {0x0279, 'r'},   // ɹ turned r
    {0x027A, 'r'},   // ɺ turned r with long leg
    {0x027B, 'r'},   // ɻ turned r with hook
    {0x027D, 'r'},   // ɽ r with tail
    {0x027E, 'r'},   // ɾ r with fishhook
    {0x0280, 'R'},   // ʀ small capital R
    {0x0281, 'R'},   // ʁ inverted small capital R
    {0x0282, 's'},   // ʂ s with hook
    {0x0283, 's'},   // ʃ esh
    {0x0288, 't'},   // ʈ t with retroflex hook
    {0x0289, 'u'},   // ʉ u bar
    {0x028A, 'U'},   // ʊ upsilon -- closer to a capital U than a lowercase one
    {0x028B, 'v'},   // ʋ v with hook
    {0x028C, 'v'},   // ʌ turned v
    {0x028D, 'w'},   // ʍ turned w
    {0x028E, 'y'},   // ʎ turned y
    {0x0290, 'z'},   // ʐ z with retroflex hook
    {0x0291, 'z'},   // ʑ z with curl
    {0x0292, 'z'},   // ʒ ezh
    {0x0294, '?'},   // ʔ glottal stop
    {0x0295, '?'},   // ʕ pharyngeal voiced fricative
    {0x029D, 'j'},   // ʝ j with crossed tail
    {0x029F, 'L'},   // ʟ small capital L
    {0x02A3, 'd'},   // ʣ dz digraph
    {0x02A4, 'j'},   // ʤ dezh digraph
    {0x02A5, 'd'},   // ʥ dz digraph with curl
    {0x02A6, 't'},   // ʦ ts digraph
    {0x02A7, 'c'},   // ʧ tesh digraph
    {0x02A8, 't'},   // ʨ tc digraph with curl
    {0x02B0, 'h'},   // ʰ modifier h
    {0x02B2, 'j'},   // ʲ modifier j
    {0x02B7, 'w'},   // ʷ modifier w
    {0x02BC, '\''},  // ʼ modifier apostrophe
    {0x02C8, '\''},  // ˈ primary stress -- a vertical tick
    {0x02CC, ','},   // ˌ secondary stress -- a low tick
    {0x02D0, ':'},   // ː length mark -- a triangular colon
    {0x02D1, ':'},   // ˑ half-length mark
    {0x02E1, 'l'},   // ˡ modifier l
    // Greek letters a dictionary borrows. Only where the Latin shape really is
    // the same letterform -- lambda is a capital A without its crossbar.
    {0x039B, 'A'},  // Λ capital lamda
    // Latin letters with a mark below, from Latin Extended Additional. That
    // block is not in the built-in intervals at all (only its Vietnamese
    // subrange is), so unlike an accented Latin-1 letter these really do arrive
    // here, and the base letter is far better than a box.
    {0x1E37, 'l'},  // ḷ l with dot below
    {0x1E71, 't'},  // ṱ t with circumflex below
    // Arrows and math operators. Noto Sans -- the font dictionary definitions
    // are pinned to -- carries none of this block (U+2212 aside), and
    // dictionaries lean on it structurally rather than decoratively: in PONS
    // En-De the tilde operator alone stands in for the headword 107k times.
    {0x2190, '<'},  // <- leftwards
    {0x2191, '^'},  // ^ upwards
    {0x2192, '>'},  // -> rightwards
    {0x2193, 'v'},  // v downwards
    {0x21C6, '>'},  // leftwards over rightwards
    {0x222B, 'f'},  // integral -- historically a long s, conventionally an f
    {0x223C, '~'},  // tilde operator -- an ASCII tilde is the same shape
    {0x2248, '~'},  // almost equal to
    // Box Drawing. No built-in font carries U+2500-U+257F; the MD reader's
    // table rows use the light vertical as a cell divider (#471), and code
    // blocks lean on the light set for tree listings. ASCII keeps the shape.
    {0x2500, '-'},  // ─ light horizontal
    {0x2502, '|'},  // │ light vertical
    {0x250C, '+'},  // ┌ light down and right
    {0x2510, '+'},  // ┐ light down and left
    {0x2514, '+'},  // └ light up and right
    {0x2518, '+'},  // ┘ light up and left
    {0x251C, '+'},  // ├ light vertical and right
    {0x2524, '+'},  // ┤ light vertical and left
    {0x252C, '+'},  // ┬ light down and horizontal
    {0x2534, '+'},  // ┴ light up and horizontal
    {0x253C, '+'},  // ┼ light vertical and horizontal
    // Bullet shapes a dictionary uses to separate senses. U+2022 is in the
    // General Punctuation range every font here carries.
    {0x25AA, 0x2022},  // black small square
    {0x25AB, 0x2022},  // ▫ white small square
    {0x25C6, 0x2022},  // ◆ black diamond
    {0x25CF, 0x2022},  // ● black circle
};

constexpr size_t FALLBACK_COUNT = sizeof(FALLBACKS) / sizeof(FALLBACKS[0]);

// The lookup below binary-searches this table, and the table is maintained by
// hand -- so prove it is ordered rather than trusting whoever edits it next.
constexpr bool fallbacksAreSorted() {
  for (size_t i = 1; i < FALLBACK_COUNT; i++) {
    if (FALLBACKS[i - 1].from >= FALLBACKS[i].from) return false;
  }
  return true;
}
static_assert(fallbacksAreSorted(), "FALLBACKS must be sorted by `from` and free of duplicates");

}  // namespace

uint32_t fallbackGlyphCodepoint(const uint32_t cp) {
  // Everything in the table is BMP and above ASCII; reject the common case
  // without touching it.
  if (cp < 0x0250 || cp > 0xFFFF) return cp;

  size_t lo = 0;
  size_t hi = FALLBACK_COUNT;
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (FALLBACKS[mid].from < cp) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo < FALLBACK_COUNT && FALLBACKS[lo].from == cp) return FALLBACKS[lo].to;
  return cp;
}
