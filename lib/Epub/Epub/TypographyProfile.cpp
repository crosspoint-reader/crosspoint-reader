#include "TypographyProfile.h"

#include "hyphenation/Hyphenator.h"

namespace {

// The tag lowercased, with '_' read as '-' (zh_TW is common in OPF files).
std::string normalised(const std::string& tag) {
  std::string out;
  out.reserve(tag.size());
  for (char c : tag) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (c == '_') c = '-';
    out.push_back(c);
  }
  return out;
}

bool hasPrimary(const std::string& tag, const char* primary) {
  const std::string p = primary;
  return tag.compare(0, p.size(), p) == 0 && (tag.size() == p.size() || tag[p.size()] == '-');
}

bool hasSubtag(const std::string& tag, const char* subtag) {
  const std::string needle = std::string("-") + subtag;
  for (size_t at = tag.find(needle); at != std::string::npos; at = tag.find(needle, at + 1)) {
    const size_t end = at + needle.size();
    if (end == tag.size() || tag[end] == '-') return true;
  }
  return false;
}

// Chinese written in traditional characters: an explicit script subtag decides, else the region (Taiwan, Hong
// Kong, Macao). Classical Chinese (lzh) is traditional as well. As KOReader's TextLangCfg.
bool isTraditionalChinese(const std::string& tag) {
  if (hasPrimary(tag, "lzh")) return true;
  if (hasSubtag(tag, "hant")) return true;
  if (hasSubtag(tag, "hans")) return false;
  return hasSubtag(tag, "tw") || hasSubtag(tag, "hk") || hasSubtag(tag, "mo");
}

}  // namespace

TypographyProfile TypographyProfile::forLanguage(const std::string& tag) {
  const std::string t = normalised(tag);
  TypographyProfile p;
  p.hyphenator = Hyphenator::forLanguage(tag);

  const bool chinese = hasPrimary(t, "zh") || hasPrimary(t, "lzh");
  if (chinese || hasPrimary(t, "ja")) {
    p.breaking = Breaking::BetweenCharacters;
    p.justify = Justify::BetweenCharacters;
    p.punctuation = hasPrimary(t, "ja")       ? Punctuation::Japanese
                    : isTraditionalChinese(t) ? Punctuation::TraditionalChinese
                                              : Punctuation::SimplifiedChinese;
    p.cjkLatinGap = true;
    p.wholeEmGrid = true;
    p.verticalAllowed = true;
  } else if (hasPrimary(t, "ko")) {
    // Korean spaces its words and uses Western punctuation (KLREQ): the Simplified Chinese table only meets
    // the rare CJK mark, and there is no grid and no CJK-Latin gap.
    p.breaking = Breaking::KoreanWords;
  }
  return p;
}
