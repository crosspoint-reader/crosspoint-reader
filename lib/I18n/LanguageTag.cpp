#include "LanguageTag.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string_view>

#include "I18nKeys.h"

namespace {

// EPUBs may write dc:language as ISO 639-2 ("eng", "fre", "fra") or ISO 639-1
// ("en"), so both forms must land in one Library group. Both the bibliographic
// and terminological ISO 639-2 variants are mapped. Source: Library of Congress
// registration table.
struct Iso639Mapping {
  char iso639_2[4];
  char iso639_1[3];
};
constexpr Iso639Mapping ISO_639_MAPPINGS[] = {
    {"aar", "aa"}, {"abk", "ab"}, {"afr", "af"}, {"aka", "ak"}, {"alb", "sq"}, {"amh", "am"}, {"ara", "ar"},
    {"arg", "an"}, {"arm", "hy"}, {"asm", "as"}, {"ava", "av"}, {"ave", "ae"}, {"aym", "ay"}, {"aze", "az"},
    {"bak", "ba"}, {"bam", "bm"}, {"baq", "eu"}, {"bel", "be"}, {"ben", "bn"}, {"bis", "bi"}, {"bod", "bo"},
    {"bos", "bs"}, {"bre", "br"}, {"bul", "bg"}, {"bur", "my"}, {"cat", "ca"}, {"ces", "cs"}, {"cha", "ch"},
    {"che", "ce"}, {"chi", "zh"}, {"chu", "cu"}, {"chv", "cv"}, {"cor", "kw"}, {"cos", "co"}, {"cre", "cr"},
    {"cym", "cy"}, {"cze", "cs"}, {"dan", "da"}, {"deu", "de"}, {"div", "dv"}, {"dut", "nl"}, {"dzo", "dz"},
    {"ell", "el"}, {"eng", "en"}, {"epo", "eo"}, {"est", "et"}, {"eus", "eu"}, {"ewe", "ee"}, {"fao", "fo"},
    {"fas", "fa"}, {"fij", "fj"}, {"fin", "fi"}, {"fra", "fr"}, {"fre", "fr"}, {"fry", "fy"}, {"ful", "ff"},
    {"geo", "ka"}, {"ger", "de"}, {"gla", "gd"}, {"gle", "ga"}, {"glg", "gl"}, {"glv", "gv"}, {"gre", "el"},
    {"grn", "gn"}, {"guj", "gu"}, {"hat", "ht"}, {"hau", "ha"}, {"heb", "he"}, {"her", "hz"}, {"hin", "hi"},
    {"hmo", "ho"}, {"hrv", "hr"}, {"hun", "hu"}, {"hye", "hy"}, {"ibo", "ig"}, {"ice", "is"}, {"ido", "io"},
    {"iii", "ii"}, {"iku", "iu"}, {"ile", "ie"}, {"ina", "ia"}, {"ind", "id"}, {"ipk", "ik"}, {"isl", "is"},
    {"ita", "it"}, {"jav", "jv"}, {"jpn", "ja"}, {"kal", "kl"}, {"kan", "kn"}, {"kas", "ks"}, {"kat", "ka"},
    {"kau", "kr"}, {"kaz", "kk"}, {"khm", "km"}, {"kik", "ki"}, {"kin", "rw"}, {"kir", "ky"}, {"kom", "kv"},
    {"kon", "kg"}, {"kor", "ko"}, {"kua", "kj"}, {"kur", "ku"}, {"lao", "lo"}, {"lat", "la"}, {"lav", "lv"},
    {"lim", "li"}, {"lin", "ln"}, {"lit", "lt"}, {"ltz", "lb"}, {"lub", "lu"}, {"lug", "lg"}, {"mac", "mk"},
    {"mah", "mh"}, {"mal", "ml"}, {"mao", "mi"}, {"mar", "mr"}, {"may", "ms"}, {"mkd", "mk"}, {"mlg", "mg"},
    {"mlt", "mt"}, {"mon", "mn"}, {"mri", "mi"}, {"msa", "ms"}, {"mya", "my"}, {"nau", "na"}, {"nav", "nv"},
    {"nbl", "nr"}, {"nde", "nd"}, {"ndo", "ng"}, {"nep", "ne"}, {"nld", "nl"}, {"nno", "nn"}, {"nob", "nb"},
    {"nor", "no"}, {"nya", "ny"}, {"oci", "oc"}, {"oji", "oj"}, {"ori", "or"}, {"orm", "om"}, {"oss", "os"},
    {"pan", "pa"}, {"per", "fa"}, {"pli", "pi"}, {"pol", "pl"}, {"por", "pt"}, {"pus", "ps"}, {"que", "qu"},
    {"roh", "rm"}, {"ron", "ro"}, {"rum", "ro"}, {"run", "rn"}, {"rus", "ru"}, {"sag", "sg"}, {"san", "sa"},
    {"sin", "si"}, {"slk", "sk"}, {"slo", "sk"}, {"slv", "sl"}, {"sme", "se"}, {"smo", "sm"}, {"sna", "sn"},
    {"snd", "sd"}, {"som", "so"}, {"sot", "st"}, {"spa", "es"}, {"sqi", "sq"}, {"srd", "sc"}, {"srp", "sr"},
    {"ssw", "ss"}, {"sun", "su"}, {"swa", "sw"}, {"swe", "sv"}, {"tah", "ty"}, {"tam", "ta"}, {"tat", "tt"},
    {"tel", "te"}, {"tgk", "tg"}, {"tgl", "tl"}, {"tha", "th"}, {"tib", "bo"}, {"tir", "ti"}, {"ton", "to"},
    {"tsn", "tn"}, {"tso", "ts"}, {"tuk", "tk"}, {"tur", "tr"}, {"twi", "tw"}, {"uig", "ug"}, {"ukr", "uk"},
    {"urd", "ur"}, {"uzb", "uz"}, {"ven", "ve"}, {"vie", "vi"}, {"vol", "vo"}, {"wel", "cy"}, {"wln", "wa"},
    {"wol", "wo"}, {"xho", "xh"}, {"yid", "yi"}, {"yor", "yo"}, {"zha", "za"}, {"zho", "zh"}, {"zul", "zu"},
};

constexpr bool codeLess(const char (&a)[4], const char (&b)[4]) {
  for (size_t i = 0; i < 3; i++) {
    if (a[i] != b[i]) return a[i] < b[i];
  }
  return false;
}

constexpr bool mappingsStrictlyAscending() {
  for (size_t i = 1; i < std::size(ISO_639_MAPPINGS); i++) {
    if (!codeLess(ISO_639_MAPPINGS[i - 1].iso639_2, ISO_639_MAPPINGS[i].iso639_2)) return false;
  }
  return true;
}
static_assert(mappingsStrictlyAscending(), "ISO_639_MAPPINGS must be sorted by iso639_2 without duplicates");

// eight primary-subtag letters plus the terminating NUL
constexpr size_t PRIMARY_SUBTAG_BUFFER_SIZE = 9;
static_assert(LANGUAGE_TAG_BUFFER_SIZE >= PRIMARY_SUBTAG_BUFFER_SIZE, "a full tag buffer holds a primary subtag");

// ISO 639-2 codes for uncoded, multiple, undetermined or non-linguistic content
constexpr const char* PLACEHOLDER_CODES[] = {"mis", "mul", "und", "zxx"};

bool isPlaceholderCode(const char* lowered) {
  return std::any_of(std::begin(PLACEHOLDER_CODES), std::end(PLACEHOLDER_CODES),
                     [lowered](const char* code) { return strcmp(code, lowered) == 0; });
}

constexpr bool isAsciiWhitespace(const char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

bool asciiEqualsIgnoreCase(const std::string_view a, const char* b) {
  size_t i = 0;
  for (; i < a.size() && b[i] != '\0'; i++) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  }
  return i == a.size() && b[i] == '\0';
}

std::string_view trimmed(std::string_view text) {
  while (!text.empty() && isAsciiWhitespace(text.front())) text.remove_prefix(1);
  while (!text.empty() && isAsciiWhitespace(text.back())) text.remove_suffix(1);
  return text;
}

// Lowercase the subtag before the first '-' or '_' and map ISO 639-2 to ISO
// 639-1. `rest` is what follows the separator. Rejects anything but 1-8 ASCII
// letters, and placeholder codes.
bool normalisePrimarySubtag(const std::string_view text, char (&out)[PRIMARY_SUBTAG_BUFFER_SIZE],
                            std::string_view& rest) {
  rest = {};
  const size_t primaryLength = std::min(text.find_first_of("-_"), text.size());
  if (primaryLength == 0 || primaryLength >= sizeof(out)) return false;
  for (size_t i = 0; i < primaryLength; i++) {
    const auto c = static_cast<unsigned char>(text[i]);
    if (c >= 0x80 || !std::isalpha(c)) return false;
    out[i] = static_cast<char>(std::tolower(c));
  }
  out[primaryLength] = '\0';
  if (isPlaceholderCode(out)) return false;

  const auto mapping =
      std::lower_bound(std::begin(ISO_639_MAPPINGS), std::end(ISO_639_MAPPINGS), out,
                       [](const Iso639Mapping& entry, const char* code) { return strcmp(entry.iso639_2, code) < 0; });
  if (mapping != std::end(ISO_639_MAPPINGS) && strcmp(mapping->iso639_2, out) == 0) {
    memcpy(out, mapping->iso639_1, sizeof(mapping->iso639_1));
  }
  if (primaryLength < text.size()) rest = text.substr(primaryLength + 1);
  return true;
}

std::string_view firstSubtag(const std::string_view tags) { return tags.substr(0, tags.find_first_of("-_")); }

// The primary subtag is already lowercase; match the second without case sensitivity.
const char* distinguishedTag(const char* primary, const std::string_view second) {
  const size_t primaryLength = strlen(primary);
  for (uint8_t i = 0; i < getLanguageCount(); i++) {
    const char* candidate = LANGUAGE_BCP47[i];
    if (strncmp(candidate, primary, primaryLength) != 0 || candidate[primaryLength] != '-') continue;
    if (asciiEqualsIgnoreCase(second, candidate + primaryLength + 1)) return candidate;
  }
  return nullptr;
}

}  // namespace

bool normaliseLanguageTag(const std::string_view tag, char* out, const size_t outSize) {
  if (out == nullptr || outSize == 0) return false;
  out[0] = '\0';
  if (outSize < LANGUAGE_TAG_BUFFER_SIZE) return false;

  char primary[PRIMARY_SUBTAG_BUFFER_SIZE];
  std::string_view rest;
  if (!normalisePrimarySubtag(trimmed(tag), primary, rest)) return false;

  const std::string_view second = firstSubtag(rest);
  const char* distinguished = second.empty() ? nullptr : distinguishedTag(primary, second);
  const char* result = distinguished != nullptr && strlen(distinguished) < outSize ? distinguished : primary;
  strncpy(out, result, outSize - 1);
  out[outSize - 1] = '\0';
  return true;
}

const char* languageNameForTag(const char* tag) {
  if (tag == nullptr || tag[0] == '\0') return nullptr;
  for (uint8_t i = 0; i < getLanguageCount(); i++) {
    if (strcmp(tag, LANGUAGE_BCP47[i]) == 0) return LANGUAGE_NAMES[i];
  }
  return nullptr;
}
