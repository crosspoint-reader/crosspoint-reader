// from
// https://github.com/atomic14/diy-esp32-epub-reader/blob/2c2f57fdd7e2a788d14a0bcb26b9e845a47aac42/lib/Epub/RubbishHtmlParser/htmlEntities.cpp

#include "htmlEntities.h"

#include <cstring>

const int MAX_ENTITY_LENGTH = 10;

// Use book: entities_ww2.epub to test this (Page 7: Entities parser test)
// Note the supported keys are only in lowercase
struct EntityPair {
  const char* key;
  const char* value;
};

static const EntityPair ENTITY_LOOKUP[] = {
    {"&quot;", "\""},  {"&frasl;", "⁄"},   {"&amp;", "&"},         {"&lt;", "<"},     {"&gt;", ">"},
    {"&Agrave;", "À"}, {"&Aacute;", "Á"},  {"&Acirc;", "Â"},       {"&Atilde;", "Ã"}, {"&Auml;", "Ä"},
    {"&Aring;", "Å"},  {"&AElig;", "Æ"},   {"&Ccedil;", "Ç"},      {"&Egrave;", "È"}, {"&Eacute;", "É"},
    {"&Ecirc;", "Ê"},  {"&Euml;", "Ë"},    {"&Igrave;", "Ì"},      {"&Iacute;", "Í"}, {"&Icirc;", "Î"},
    {"&Iuml;", "Ï"},   {"&ETH;", "Ð"},     {"&Ntilde;", "Ñ"},      {"&Ograve;", "Ò"}, {"&Oacute;", "Ó"},
    {"&Ocirc;", "Ô"},  {"&Otilde;", "Õ"},  {"&Ouml;", "Ö"},        {"&Oslash;", "Ø"}, {"&Ugrave;", "Ù"},
    {"&Uacute;", "Ú"}, {"&Ucirc;", "Û"},   {"&Uuml;", "Ü"},        {"&Yacute;", "Ý"}, {"&THORN;", "Þ"},
    {"&szlig;", "ß"},  {"&agrave;", "à"},  {"&aacute;", "á"},      {"&acirc;", "â"},  {"&atilde;", "ã"},
    {"&auml;", "ä"},   {"&aring;", "å"},   {"&aelig;", "æ"},       {"&ccedil;", "ç"}, {"&egrave;", "è"},
    {"&eacute;", "é"}, {"&ecirc;", "ê"},   {"&euml;", "ë"},        {"&igrave;", "ì"}, {"&iacute;", "í"},
    {"&icirc;", "î"},  {"&iuml;", "ï"},    {"&eth;", "ð"},         {"&ntilde;", "ñ"}, {"&ograve;", "ò"},
    {"&oacute;", "ó"}, {"&ocirc;", "ô"},   {"&otilde;", "õ"},      {"&ouml;", "ö"},   {"&oslash;", "ø"},
    {"&ugrave;", "ù"}, {"&uacute;", "ú"},  {"&ucirc;", "û"},       {"&uuml;", "ü"},   {"&yacute;", "ý"},
    {"&thorn;", "þ"},  {"&yuml;", "ÿ"},    {"&nbsp;", "\xC2\xA0"}, {"&iexcl;", "¡"},  {"&cent;", "¢"},
    {"&pound;", "£"},  {"&curren;", "¤"},  {"&yen;", "¥"},         {"&brvbar;", "¦"}, {"&sect;", "§"},
    {"&uml;", "¨"},    {"&copy;", "©"},    {"&ordf;", "ª"},        {"&laquo;", "«"},  {"&not;", "¬"},
    {"&shy;", "­"},    {"&reg;", "®"},     {"&macr;", "¯"},        {"&deg;", "°"},    {"&plusmn;", "±"},
    {"&sup2;", "²"},   {"&sup3;", "³"},    {"&acute;", "´"},       {"&micro;", "µ"},  {"&para;", "¶"},
    {"&cedil;", "¸"},  {"&sup1;", "¹"},    {"&ordm;", "º"},        {"&raquo;", "»"},  {"&frac14;", "¼"},
    {"&frac12;", "½"}, {"&frac34;", "¾"},  {"&iquest;", "¿"},      {"&times;", "×"},  {"&divide;", "÷"},
    {"&forall;", "∀"}, {"&part;", "∂"},    {"&exist;", "∃"},       {"&empty;", "∅"},  {"&nabla;", "∇"},
    {"&isin;", "∈"},   {"&notin;", "∉"},   {"&ni;", "∋"},          {"&prod;", "∏"},   {"&sum;", "∑"},
    {"&minus;", "−"},  {"&lowast;", "∗"},  {"&radic;", "√"},       {"&prop;", "∝"},   {"&infin;", "∞"},
    {"&ang;", "∠"},    {"&and;", "∧"},     {"&or;", "∨"},          {"&cap;", "∩"},    {"&cup;", "∪"},
    {"&int;", "∫"},    {"&there4;", "∴"},  {"&sim;", "∼"},         {"&cong;", "≅"},   {"&asymp;", "≈"},
    {"&ne;", "≠"},     {"&equiv;", "≡"},   {"&le;", "≤"},          {"&ge;", "≥"},     {"&sub;", "⊂"},
    {"&sup;", "⊃"},    {"&nsub;", "⊄"},    {"&sube;", "⊆"},        {"&supe;", "⊇"},   {"&oplus;", "⊕"},
    {"&otimes;", "⊗"}, {"&perp;", "⊥"},    {"&sdot;", "⋅"},        {"&Alpha;", "Α"},  {"&Beta;", "Β"},
    {"&Gamma;", "Γ"},  {"&Delta;", "Δ"},   {"&Epsilon;", "Ε"},     {"&Zeta;", "Ζ"},   {"&Eta;", "Η"},
    {"&Theta;", "Θ"},  {"&Iota;", "Ι"},    {"&Kappa;", "Κ"},       {"&Lambda;", "Λ"}, {"&Mu;", "Μ"},
    {"&Nu;", "Ν"},     {"&Xi;", "Ξ"},      {"&Omicron;", "Ο"},     {"&Pi;", "Π"},     {"&Rho;", "Ρ"},
    {"&Sigma;", "Σ"},  {"&Tau;", "Τ"},     {"&Upsilon;", "Υ"},     {"&Phi;", "Φ"},    {"&Chi;", "Χ"},
    {"&Psi;", "Ψ"},    {"&Omega;", "Ω"},   {"&alpha;", "α"},       {"&beta;", "β"},   {"&gamma;", "γ"},
    {"&delta;", "δ"},  {"&epsilon;", "ε"}, {"&zeta;", "ζ"},        {"&eta;", "η"},    {"&theta;", "θ"},
    {"&iota;", "ι"},   {"&kappa;", "κ"},   {"&lambda;", "λ"},      {"&mu;", "μ"},     {"&nu;", "ν"},
    {"&xi;", "ξ"},     {"&omicron;", "ο"}, {"&pi;", "π"},          {"&rho;", "ρ"},    {"&sigmaf;", "ς"},
    {"&sigma;", "σ"},  {"&tau;", "τ"},     {"&upsilon;", "υ"},     {"&phi;", "φ"},    {"&chi;", "χ"},
    {"&psi;", "ψ"},    {"&omega;", "ω"},   {"&thetasym;", "ϑ"},    {"&upsih;", "ϒ"},  {"&piv;", "ϖ"},
    {"&OElig;", "Œ"},  {"&oelig;", "œ"},   {"&Scaron;", "Š"},      {"&scaron;", "š"}, {"&Yuml;", "Ÿ"},
    {"&fnof;", "ƒ"},   {"&circ;", "ˆ"},    {"&tilde;", "˜"},       {"&ensp;", ""},    {"&emsp;", ""},
    {"&thinsp;", ""},  {"&zwnj;", "‌"},  {"&zwj;", "‍"},       {"&lrm;", "‎"},  {"&rlm;", "‏"},
    {"&ndash;", "–"},  {"&mdash;", "—"},   {"&lsquo;", "‘"},       {"&rsquo;", "’"},  {"&sbquo;", "‚"},
    {"&ldquo;", "“"},  {"&rdquo;", "”"},   {"&bdquo;", "„"},       {"&dagger;", "†"}, {"&Dagger;", "‡"},
    {"&bull;", "•"},   {"&hellip;", "…"},  {"&permil;", "‰"},      {"&prime;", "′"},  {"&Prime;", "″"},
    {"&lsaquo;", "‹"}, {"&rsaquo;", "›"},  {"&oline;", "‾"},       {"&euro;", "€"},   {"&trade;", "™"},
    {"&larr;", "←"},   {"&uarr;", "↑"},    {"&rarr;", "→"},        {"&darr;", "↓"},   {"&harr;", "↔"},
    {"&crarr;", "↵"},  {"&lceil;", "⌈"},   {"&rceil;", "⌉"},       {"&lfloor;", "⌊"}, {"&rfloor;", "⌋"},
    {"&loz;", "◊"},    {"&spades;", "♠"},  {"&clubs;", "♣"},       {"&hearts;", "♥"}, {"&diams;", "♦"}};

static const size_t ENTITY_LOOKUP_COUNT = sizeof(ENTITY_LOOKUP) / sizeof(ENTITY_LOOKUP[0]);

// converts from a unicode code point to the utf8 equivalent
void convert_to_utf8(const int code, std::string& res) {
  // convert to a utf8 sequence
  if (code < 0x80) {
    res += static_cast<char>(code);
  } else if (code < 0x800) {
    res += static_cast<char>(0xc0 | (code >> 6));
    res += static_cast<char>(0x80 | (code & 0x3f));
  } else if (code < 0x10000) {
    res += static_cast<char>(0xe0 | (code >> 12));
    res += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
    res += static_cast<char>(0x80 | (code & 0x3f));
  } else if (code < 0x200000) {
    res += static_cast<char>(0xf0 | (code >> 18));
    res += static_cast<char>(0x80 | ((code >> 12) & 0x3f));
    res += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
    res += static_cast<char>(0x80 | (code & 0x3f));
  } else if (code < 0x4000000) {
    res += static_cast<char>(0xf8 | (code >> 24));
    res += static_cast<char>(0x80 | ((code >> 18) & 0x3f));
    res += static_cast<char>(0x80 | ((code >> 12) & 0x3f));
    res += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
    res += static_cast<char>(0x80 | (code & 0x3f));
  } else if (code < 0x80000000) {
    res += static_cast<char>(0xfc | (code >> 30));
    res += static_cast<char>(0x80 | ((code >> 24) & 0x3f));
    res += static_cast<char>(0x80 | ((code >> 18) & 0x3f));
    res += static_cast<char>(0x80 | ((code >> 12) & 0x3f));
    res += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
  }
}

// handles numeric entities - e.g. &#1234; or &#x1234;
bool process_numeric_entity(const std::string& entity, std::string& res) {
  int code = 0;
  // is it hex?
  if (entity[2] == 'x' || entity[2] == 'X') {
    // parse the hex code
    code = strtol(entity.substr(3, entity.size() - 3).c_str(), nullptr, 16);
  } else {
    code = strtol(entity.substr(2, entity.size() - 3).c_str(), nullptr, 10);
  }
  if (code != 0) {
    // special handling for nbsp
    if (code == 0xA0) {
      res += " ";
    } else {
      convert_to_utf8(code, res);
    }
    return true;
  }
  return false;
}

// handles named entities - e.g. &amp;
bool process_string_entity(const std::string& entity, std::string& res) {
  // it's a named entity - find it in the lookup table
  // find it in the map
  for (size_t i = 0; i < ENTITY_LOOKUP_COUNT; i++) {
    if (strcmp(entity.c_str(), ENTITY_LOOKUP[i].key) == 0) {
      res += ENTITY_LOOKUP[i].value;
      return true;
    }
  }
  return false;
}

// replace all the entities in the string
std::string replaceHtmlEntities(const char* text) {
  std::string res;
  res.reserve(strlen(text));
  for (int i = 0; i < strlen(text); ++i) {
    bool flag = false;
    // do we have a potential entity?
    if (text[i] == '&') {
      // find the end of the entity
      int j = i + 1;
      while (j < strlen(text) && text[j] != ';' && j - i < MAX_ENTITY_LENGTH) {
        j++;
      }
      if (j - i > 2) {
        char entity[j - i + 1];
        strncpy(entity, text + i, j - i);
        entity[j - i] = '\0';
        // is it a numeric code?
        if (entity[1] == '#') {
          flag = process_numeric_entity(entity, res);
        } else {
          flag = process_string_entity(entity, res);
        }
        // skip past the entity if we successfully decoded it
        if (flag) {
          i = j;
        }
      }
    }
    if (!flag) {
      res += text[i];
    }
  }
  return res;
}

// Lookup a single HTML entity and return its UTF-8 value
const char* lookupHtmlEntity(const char* entity, int len) {
  for (size_t i = 0; i < ENTITY_LOOKUP_COUNT; i++) {
    const char* key = ENTITY_LOOKUP[i].key;
    const size_t keyLen = strlen(key);
    if (static_cast<size_t>(len) == keyLen && memcmp(entity, key, keyLen) == 0) {
      return ENTITY_LOOKUP[i].value;
    }
  }

  return nullptr;  // Entity not found
}
