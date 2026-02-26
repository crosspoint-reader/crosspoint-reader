// based on
// https://github.com/atomic14/diy-esp32-epub-reader/blob/2c2f57fdd7e2a788d14a0bcb26b9e845a47aac42/lib/Epub/RubbishHtmlParser/htmlEntities.cpp

#include "htmlEntities.h"

#include <cstring>

struct EntityPair {
  const char* key;
  const char* value;
};

// Sorted lexicographically by key to allow binary search.
static constexpr EntityPair ENTITY_LOOKUP[] = {
    {"&AElig;", "Æ"},    {"&Aacute;", "Á"},     {"&Acirc;", "Â"},      {"&Agrave;", "À"},   {"&Alpha;", "Α"},
    {"&Aring;", "Å"},    {"&Atilde;", "Ã"},     {"&Auml;", "Ä"},       {"&Beta;", "Β"},     {"&Ccedil;", "Ç"},
    {"&Chi;", "Χ"},      {"&Dagger;", "‡"},     {"&Delta;", "Δ"},      {"&ETH;", "Ð"},      {"&Eacute;", "É"},
    {"&Ecirc;", "Ê"},    {"&Egrave;", "È"},     {"&Epsilon;", "Ε"},    {"&Eta;", "Η"},      {"&Euml;", "Ë"},
    {"&Gamma;", "Γ"},    {"&Iacute;", "Í"},     {"&Icirc;", "Î"},      {"&Igrave;", "Ì"},   {"&Iota;", "Ι"},
    {"&Iuml;", "Ï"},     {"&Kappa;", "Κ"},      {"&Lambda;", "Λ"},     {"&Mu;", "Μ"},       {"&Ntilde;", "Ñ"},
    {"&Nu;", "Ν"},       {"&OElig;", "Œ"},      {"&Oacute;", "Ó"},     {"&Ocirc;", "Ô"},    {"&Ograve;", "Ò"},
    {"&Omega;", "Ω"},    {"&Omicron;", "Ο"},    {"&Oslash;", "Ø"},     {"&Otilde;", "Õ"},   {"&Ouml;", "Ö"},
    {"&Phi;", "Φ"},      {"&Pi;", "Π"},         {"&Prime;", "″"},      {"&Psi;", "Ψ"},      {"&Rho;", "Ρ"},
    {"&Scaron;", "Š"},   {"&Sigma;", "Σ"},      {"&THORN;", "Þ"},      {"&Tau;", "Τ"},      {"&Theta;", "Θ"},
    {"&Uacute;", "Ú"},   {"&Ucirc;", "Û"},      {"&Ugrave;", "Ù"},     {"&Upsilon;", "Υ"},  {"&Uuml;", "Ü"},
    {"&Xi;", "Ξ"},       {"&Yacute;", "Ý"},     {"&Yuml;", "Ÿ"},       {"&Zeta;", "Ζ"},     {"&aacute;", "á"},
    {"&acirc;", "â"},    {"&acute;", "´"},      {"&aelig;", "æ"},      {"&agrave;", "à"},   {"&alpha;", "α"},
    {"&amp;", "&"},      {"&and;", "∧"},        {"&ang;", "∠"},        {"&aring;", "å"},    {"&asymp;", "≈"},
    {"&atilde;", "ã"},   {"&auml;", "ä"},       {"&bdquo;", "„"},      {"&beta;", "β"},     {"&brvbar;", "¦"},
    {"&bull;", "•"},     {"&cap;", "∩"},        {"&ccedil;", "ç"},     {"&cedil;", "¸"},    {"&cent;", "¢"},
    {"&chi;", "χ"},      {"&circ;", "ˆ"},       {"&clubs;", "♣"},      {"&cong;", "≅"},     {"&copy;", "©"},
    {"&crarr;", "↵"},    {"&cup;", "∪"},        {"&curren;", "¤"},     {"&dagger;", "†"},   {"&darr;", "↓"},
    {"&deg;", "°"},      {"&delta;", "δ"},      {"&diams;", "♦"},      {"&divide;", "÷"},   {"&eacute;", "é"},
    {"&ecirc;", "ê"},    {"&egrave;", "è"},     {"&empty;", "∅"},      {"&emsp;", " "},     {"&ensp;", " "},
    {"&epsilon;", "ε"},  {"&equiv;", "≡"},      {"&eta;", "η"},        {"&eth;", "ð"},      {"&euml;", "ë"},
    {"&euro;", "€"},     {"&exist;", "∃"},      {"&fnof;", "ƒ"},       {"&forall;", "∀"},   {"&frac12;", "½"},
    {"&frac14;", "¼"},   {"&frac34;", "¾"},     {"&frasl;", "⁄"},      {"&gamma;", "γ"},    {"&ge;", "≥"},
    {"&gt;", ">"},       {"&harr;", "↔"},       {"&hearts;", "♥"},     {"&hellip;", "…"},   {"&iacute;", "í"},
    {"&icirc;", "î"},    {"&iexcl;", "¡"},      {"&igrave;", "ì"},     {"&infin;", "∞"},    {"&int;", "∫"},
    {"&iota;", "ι"},     {"&iquest;", "¿"},     {"&isin;", "∈"},       {"&iuml;", "ï"},     {"&kappa;", "κ"},
    {"&lambda;", "λ"},   {"&laquo;", "«"},      {"&larr;", "←"},       {"&lceil;", "⌈"},    {"&ldquo;", "\u201C"},
    {"&le;", "≤"},       {"&lfloor;", "⌊"},     {"&lowast;", "∗"},     {"&loz;", "◊"},      {"&lrm;", "\u200E"},
    {"&lsaquo;", "‹"},   {"&lsquo;", "\u2018"}, {"&lt;", "<"},         {"&macr;", "¯"},     {"&mdash;", "—"},
    {"&micro;", "µ"},    {"&minus;", "−"},      {"&mu;", "μ"},         {"&nabla;", "∇"},    {"&nbsp;", "\xC2\xA0"},
    {"&ndash;", "–"},    {"&ne;", "≠"},         {"&ni;", "∋"},         {"&not;", "¬"},      {"&notin;", "∉"},
    {"&nsub;", "⊄"},     {"&ntilde;", "ñ"},     {"&nu;", "ν"},         {"&oacute;", "ó"},   {"&ocirc;", "ô"},
    {"&oelig;", "œ"},    {"&ograve;", "ò"},     {"&oline;", "‾"},      {"&omega;", "ω"},    {"&omicron;", "ο"},
    {"&oplus;", "⊕"},    {"&or;", "∨"},         {"&ordf;", "ª"},       {"&ordm;", "º"},     {"&oslash;", "ø"},
    {"&otilde;", "õ"},   {"&otimes;", "⊗"},     {"&ouml;", "ö"},       {"&para;", "¶"},     {"&part;", "∂"},
    {"&permil;", "‰"},   {"&perp;", "⊥"},       {"&phi;", "φ"},        {"&pi;", "π"},       {"&piv;", "ϖ"},
    {"&plusmn;", "±"},   {"&pound;", "£"},      {"&prime;", "′"},      {"&prod;", "∏"},     {"&prop;", "∝"},
    {"&psi;", "ψ"},      {"&quot;", "\""},      {"&radic;", "√"},      {"&raquo;", "»"},    {"&rarr;", "→"},
    {"&rceil;", "⌉"},    {"&rdquo;", "\u201D"}, {"&reg;", "®"},        {"&rfloor;", "⌋"},   {"&rho;", "ρ"},
    {"&rlm;", "\u200F"}, {"&rsaquo;", "›"},     {"&rsquo;", "\u2019"}, {"&sbquo;", "‚"},    {"&scaron;", "š"},
    {"&sdot;", "⋅"},     {"&sect;", "§"},       {"&shy;", "\xC2\xAD"}, {"&sigma;", "σ"},    {"&sigmaf;", "ς"},
    {"&sim;", "∼"},      {"&spades;", "♠"},     {"&sub;", "⊂"},        {"&sube;", "⊆"},     {"&sum;", "∑"},
    {"&sup1;", "¹"},     {"&sup2;", "²"},       {"&sup3;", "³"},       {"&sup;", "⊃"},      {"&supe;", "⊇"},
    {"&szlig;", "ß"},    {"&tau;", "τ"},        {"&there4;", "∴"},     {"&theta;", "θ"},    {"&thetasym;", "ϑ"},
    {"&thinsp;", " "},   {"&thorn;", "þ"},      {"&tilde;", "˜"},      {"&times;", "×"},    {"&trade;", "™"},
    {"&uacute;", "ú"},   {"&uarr;", "↑"},       {"&ucirc;", "û"},      {"&ugrave;", "ù"},   {"&uml;", "¨"},
    {"&upsih;", "ϒ"},    {"&upsilon;", "υ"},    {"&uuml;", "ü"},       {"&xi;", "ξ"},       {"&yacute;", "ý"},
    {"&yen;", "¥"},      {"&yuml;", "ÿ"},       {"&zeta;", "ζ"},       {"&zwj;", "\u200D"}, {"&zwnj;", "\u200C"},
};

static const size_t ENTITY_LOOKUP_COUNT = sizeof(ENTITY_LOOKUP) / sizeof(ENTITY_LOOKUP[0]);

// Verify the table is sorted at compile time.
static constexpr int constexprStrcmp(const char* a, const char* b) {
  for (size_t i = 0;; i++) {
    if (a[i] != b[i]) return (unsigned char)a[i] < (unsigned char)b[i] ? -1 : 1;
    if (a[i] == '\0') return 0;
  }
}

static constexpr bool isTableSorted() {
  for (size_t i = 1; i < ENTITY_LOOKUP_COUNT; i++) {
    if (constexprStrcmp(ENTITY_LOOKUP[i - 1].key, ENTITY_LOOKUP[i].key) >= 0) return false;
  }
  return true;
}
static_assert(isTableSorted(), "ENTITY_LOOKUP must be sorted lexicographically by key");

// Lookup a single HTML entity and return its UTF-8 value.
const char* lookupHtmlEntity(const char* entity, size_t len) {
  if (entity == nullptr || len == 0) return nullptr;

  size_t lo = 0;
  size_t hi = ENTITY_LOOKUP_COUNT;

  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const char* key = ENTITY_LOOKUP[mid].key;
    const size_t keyLen = strlen(key);
    const size_t cmpLen = (len < keyLen) ? len : keyLen;
    int cmp = memcmp(entity, key, cmpLen);
    if (cmp == 0) {
      // safety net: if prefix equal, shorter string is considered smaller
      if (len < keyLen)
        cmp = -1;
      else if (len > keyLen)
        cmp = 1;
      else
        cmp = 0;
    }

    if (cmp == 0) return ENTITY_LOOKUP[mid].value;
    if (cmp < 0)
      hi = mid;
    else
      lo = mid + 1;
  }

  return nullptr;
}
