// Pocket Library for CrossPoint
// Copyright (C) 2026 Pocket Library contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. See LICENSE-GPL-3.0 at the repository root.

#include "ZimHtml.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace zim {
namespace {

// ---------------------------------------------------------------- entities

struct NamedEntity {
  const char* name;
  uint32_t cp;
};

// The named entities that occur in Kiwix articles in practice (MediaWiki
// output, Wikisource transcriptions, medical texts). Sorted for binary search.
constexpr NamedEntity kEntities[] = {
    {"AElig", 0xC6},    {"Aacute", 0xC1},   {"Acirc", 0xC2},     {"Agrave", 0xC0},   {"Alpha", 0x391},
    {"Aring", 0xC5},    {"Atilde", 0xC3},   {"Auml", 0xC4},      {"Beta", 0x392},    {"Ccedil", 0xC7},
    {"Chi", 0x3A7},     {"Dagger", 0x2021}, {"Delta", 0x394},    {"ETH", 0xD0},      {"Eacute", 0xC9},
    {"Ecirc", 0xCA},    {"Egrave", 0xC8},   {"Epsilon", 0x395},  {"Eta", 0x397},     {"Euml", 0xCB},
    {"Gamma", 0x393},   {"Iacute", 0xCD},   {"Icirc", 0xCE},     {"Igrave", 0xCC},   {"Iota", 0x399},
    {"Iuml", 0xCF},     {"Kappa", 0x39A},   {"Lambda", 0x39B},   {"Mu", 0x39C},      {"Ntilde", 0xD1},
    {"Nu", 0x39D},      {"OElig", 0x152},   {"Oacute", 0xD3},    {"Ocirc", 0xD4},    {"Ograve", 0xD2},
    {"Omega", 0x3A9},   {"Omicron", 0x39F}, {"Oslash", 0xD8},    {"Otilde", 0xD5},   {"Ouml", 0xD6},
    {"Phi", 0x3A6},     {"Pi", 0x3A0},      {"Prime", 0x2033},   {"Psi", 0x3A8},     {"Rho", 0x3A1},
    {"Scaron", 0x160},  {"Sigma", 0x3A3},   {"THORN", 0xDE},     {"Tau", 0x3A4},     {"Theta", 0x398},
    {"Uacute", 0xDA},   {"Ucirc", 0xDB},    {"Ugrave", 0xD9},    {"Upsilon", 0x3A5}, {"Uuml", 0xDC},
    {"Xi", 0x39E},      {"Yacute", 0xDD},   {"Yuml", 0x178},     {"Zeta", 0x396},    {"aacute", 0xE1},
    {"acirc", 0xE2},    {"acute", 0xB4},    {"aelig", 0xE6},     {"agrave", 0xE0},   {"alpha", 0x3B1},
    {"amp", 0x26},      {"and", 0x2227},    {"ang", 0x2220},     {"apos", 0x27},     {"aring", 0xE5},
    {"asymp", 0x2248},  {"atilde", 0xE3},   {"auml", 0xE4},      {"bdquo", 0x201E},  {"beta", 0x3B2},
    {"brvbar", 0xA6},   {"bull", 0x2022},   {"cap", 0x2229},     {"ccedil", 0xE7},   {"cedil", 0xB8},
    {"cent", 0xA2},     {"chi", 0x3C7},     {"circ", 0x2C6},     {"clubs", 0x2663},  {"cong", 0x2245},
    {"copy", 0xA9},     {"crarr", 0x21B5},  {"cup", 0x222A},     {"curren", 0xA4},   {"dArr", 0x21D3},
    {"dagger", 0x2020}, {"darr", 0x2193},   {"deg", 0xB0},       {"delta", 0x3B4},   {"diams", 0x2666},
    {"divide", 0xF7},   {"eacute", 0xE9},   {"ecirc", 0xEA},     {"egrave", 0xE8},   {"empty", 0x2205},
    {"emsp", 0x2003},   {"ensp", 0x2002},   {"epsilon", 0x3B5},  {"equiv", 0x2261},  {"eta", 0x3B7},
    {"eth", 0xF0},      {"euml", 0xEB},     {"euro", 0x20AC},    {"exist", 0x2203},  {"fnof", 0x192},
    {"forall", 0x2200}, {"frac12", 0xBD},   {"frac14", 0xBC},    {"frac34", 0xBE},   {"frasl", 0x2044},
    {"gamma", 0x3B3},   {"ge", 0x2265},     {"gt", 0x3E},        {"hArr", 0x21D4},   {"harr", 0x2194},
    {"hearts", 0x2665}, {"hellip", 0x2026}, {"iacute", 0xED},    {"icirc", 0xEE},    {"iexcl", 0xA1},
    {"igrave", 0xEC},   {"infin", 0x221E},  {"int", 0x222B},     {"iota", 0x3B9},    {"iquest", 0xBF},
    {"isin", 0x2208},   {"iuml", 0xEF},     {"kappa", 0x3BA},    {"lArr", 0x21D0},   {"lambda", 0x3BB},
    {"lang", 0x27E8},   {"laquo", 0xAB},    {"larr", 0x2190},    {"lceil", 0x2308},  {"ldquo", 0x201C},
    {"le", 0x2264},     {"lfloor", 0x230A}, {"lowast", 0x2217},  {"loz", 0x25CA},    {"lrm", 0x200E},
    {"lsaquo", 0x2039}, {"lsquo", 0x2018},  {"lt", 0x3C},        {"macr", 0xAF},     {"mdash", 0x2014},
    {"micro", 0xB5},    {"middot", 0xB7},   {"minus", 0x2212},   {"mu", 0x3BC},      {"nabla", 0x2207},
    {"nbsp", 0xA0},     {"ndash", 0x2013},  {"ne", 0x2260},      {"ni", 0x220B},     {"not", 0xAC},
    {"notin", 0x2209},  {"nsub", 0x2284},   {"ntilde", 0xF1},    {"nu", 0x3BD},      {"oacute", 0xF3},
    {"ocirc", 0xF4},    {"oelig", 0x153},   {"ograve", 0xF2},    {"oline", 0x203E},  {"omega", 0x3C9},
    {"omicron", 0x3BF}, {"oplus", 0x2295},  {"or", 0x2228},      {"ordf", 0xAA},     {"ordm", 0xBA},
    {"oslash", 0xF8},   {"otilde", 0xF5},   {"otimes", 0x2297},  {"ouml", 0xF6},     {"para", 0xB6},
    {"part", 0x2202},   {"permil", 0x2030}, {"perp", 0x22A5},    {"phi", 0x3C6},     {"pi", 0x3C0},
    {"piv", 0x3D6},     {"plusmn", 0xB1},   {"pound", 0xA3},     {"prime", 0x2032},  {"prod", 0x220F},
    {"prop", 0x221D},   {"psi", 0x3C8},     {"quot", 0x22},      {"rArr", 0x21D2},   {"radic", 0x221A},
    {"rang", 0x27E9},   {"raquo", 0xBB},    {"rarr", 0x2192},    {"rceil", 0x2309},  {"rdquo", 0x201D},
    {"reg", 0xAE},      {"rfloor", 0x230B}, {"rho", 0x3C1},      {"rlm", 0x200F},    {"rsaquo", 0x203A},
    {"rsquo", 0x2019},  {"sbquo", 0x201A},  {"scaron", 0x161},   {"sdot", 0x22C5},   {"sect", 0xA7},
    {"shy", 0xAD},      {"sigma", 0x3C3},   {"sigmaf", 0x3C2},   {"sim", 0x223C},    {"spades", 0x2660},
    {"sub", 0x2282},    {"sube", 0x2286},   {"sum", 0x2211},     {"sup", 0x2283},    {"sup1", 0xB9},
    {"sup2", 0xB2},     {"sup3", 0xB3},     {"supe", 0x2287},    {"szlig", 0xDF},    {"tau", 0x3C4},
    {"there4", 0x2234}, {"theta", 0x3B8},   {"thetasym", 0x3D1}, {"thinsp", 0x2009}, {"thorn", 0xFE},
    {"tilde", 0x2DC},   {"times", 0xD7},    {"trade", 0x2122},   {"uArr", 0x21D1},   {"uacute", 0xFA},
    {"uarr", 0x2191},   {"ucirc", 0xFB},    {"ugrave", 0xF9},    {"uml", 0xA8},      {"upsih", 0x3D2},
    {"upsilon", 0x3C5}, {"uuml", 0xFC},     {"xi", 0x3BE},       {"yacute", 0xFD},   {"yen", 0xA5},
    {"yuml", 0xFF},     {"zeta", 0x3B6},    {"zwj", 0x200D},     {"zwnj", 0x200C},
};

bool isAsciiAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool isAsciiDigit(char c) { return c >= '0' && c <= '9'; }
bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool validXmlCodePoint(uint32_t cp) {
  if (cp == 0x9 || cp == 0xA || cp == 0xD) return true;
  if (cp < 0x20) return false;
  if (cp >= 0xD800 && cp <= 0xDFFF) return false;
  if (cp == 0xFFFE || cp == 0xFFFF) return false;
  return cp <= 0x10FFFF;
}

void appendUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

// Appends one code point as XML text: markup characters escaped, characters
// XML forbids dropped.
void appendXmlChar(std::string& out, uint32_t cp) {
  if (!validXmlCodePoint(cp)) return;
  switch (cp) {
    case '&':
      out += "&amp;";
      return;
    case '<':
      out += "&lt;";
      return;
    case '>':
      out += "&gt;";
      return;
    case '"':
      out += "&quot;";
      return;
    default:
      appendUtf8(out, cp);
  }
}

// Decodes one UTF-8 sequence at s[i]; advances i. Invalid bytes become U+FFFD
// one byte at a time, so a stray Latin-1 byte cannot swallow its neighbours.
uint32_t nextCodePoint(std::string_view s, size_t& i) {
  const auto b0 = static_cast<uint8_t>(s[i]);
  if (b0 < 0x80) {
    i++;
    return b0;
  }
  int extra;
  uint32_t cp;
  if ((b0 & 0xE0) == 0xC0) {
    extra = 1;
    cp = b0 & 0x1F;
  } else if ((b0 & 0xF0) == 0xE0) {
    extra = 2;
    cp = b0 & 0x0F;
  } else if ((b0 & 0xF8) == 0xF0) {
    extra = 3;
    cp = b0 & 0x07;
  } else {
    i++;
    return 0xFFFD;
  }
  if (i + static_cast<size_t>(extra) >= s.size()) {  // truncated sequence
    i++;
    return 0xFFFD;
  }
  for (int k = 1; k <= extra; k++) {
    const auto b = static_cast<uint8_t>(s[i + k]);
    if ((b & 0xC0) != 0x80) {
      i++;
      return 0xFFFD;
    }
    cp = (cp << 6) | (b & 0x3F);
  }
  static constexpr uint32_t kMin[] = {0, 0x80, 0x800, 0x10000};
  if (cp < kMin[extra] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
    i++;
    return 0xFFFD;
  }
  i += static_cast<size_t>(extra) + 1;
  return cp;
}

// Appends raw HTML text (entities still encoded) as XML text, or as plain
// UTF-8 when `xml` is false.
void appendText(std::string& out, std::string_view text, bool xml = true) {
  size_t i = 0;
  while (i < text.size()) {
    const char c = text[i];
    if (c == '&') {
      // Entity: &name; &#123; &#x1F;. The ';' is optional in HTML for some,
      // but MediaWiki always writes it; without it, keep the '&' literally.
      size_t j = i + 1;
      while (j < text.size() && j - i <= 32 && (isAsciiAlpha(text[j]) || isAsciiDigit(text[j]) || text[j] == '#')) j++;
      if (j < text.size() && text[j] == ';' && j > i + 1) {
        const uint32_t cp = decodeHtmlEntity(text.substr(i + 1, j - i - 1));
        if (cp != 0) {
          if (xml)
            appendXmlChar(out, cp);
          else if (validXmlCodePoint(cp))
            appendUtf8(out, cp);
          i = j + 1;
          continue;
        }
      }
      out += xml ? "&amp;" : "&";
      i++;
      continue;
    }
    const uint32_t cp = nextCodePoint(text, i);
    if (xml)
      appendXmlChar(out, cp);
    else if (validXmlCodePoint(cp))
      appendUtf8(out, cp);
  }
}

std::string plainText(std::string_view raw) {
  std::string out;
  appendText(out, raw, false);
  return out;
}

bool isHeadingTag(const char* t) { return t[0] == 'h' && t[1] >= '1' && t[1] <= '6' && t[2] == 0; }

// ---------------------------------------------------------------- policy

enum class Action : uint8_t {
  Keep,    // emit as `out` (same name or renamed)
  Unwrap,  // emit nothing for the tag, keep its content
  Drop,    // drop the element and everything inside it
};

struct TagRule {
  const char* name;
  Action action;
  const char* out;  // output tag for Keep
};

// Tags not listed are unwrapped. Output tags are the ones ChapterHtmlSlimParser
// lays out (h1-h6, p, li, div, br, blockquote, ul, ol, table/tr/td/th, hr,
// b/i/u/s, sup/sub, a, ruby/rt).
constexpr TagRule kRules[] = {
    {"a", Action::Keep, "a"},
    {"address", Action::Keep, "div"},
    {"area", Action::Drop, nullptr},
    {"article", Action::Keep, "div"},
    {"aside", Action::Keep, "div"},
    {"audio", Action::Drop, nullptr},
    {"b", Action::Keep, "b"},
    {"base", Action::Drop, nullptr},
    {"blockquote", Action::Keep, "blockquote"},
    {"br", Action::Keep, "br"},
    {"button", Action::Drop, nullptr},
    {"canvas", Action::Drop, nullptr},
    {"caption", Action::Keep, "p"},
    {"center", Action::Keep, "div"},
    {"cite", Action::Keep, "i"},
    {"col", Action::Drop, nullptr},
    {"colgroup", Action::Drop, nullptr},
    {"dd", Action::Keep, "blockquote"},
    {"del", Action::Keep, "s"},
    {"details", Action::Keep, "div"},
    {"dfn", Action::Keep, "i"},
    {"dialog", Action::Drop, nullptr},
    {"div", Action::Keep, "div"},
    {"dl", Action::Keep, "div"},
    {"dt", Action::Keep, "p"},
    {"em", Action::Keep, "i"},
    {"embed", Action::Drop, nullptr},
    {"figcaption", Action::Keep, "div"},
    {"figure", Action::Drop, nullptr},
    {"footer", Action::Keep, "div"},
    {"form", Action::Drop, nullptr},
    {"h1", Action::Keep, "h1"},
    {"h2", Action::Keep, "h2"},
    {"h3", Action::Keep, "h3"},
    {"h4", Action::Keep, "h4"},
    {"h5", Action::Keep, "h5"},
    {"h6", Action::Keep, "h6"},
    {"head", Action::Drop, nullptr},
    {"header", Action::Keep, "div"},
    {"hr", Action::Keep, "hr"},
    {"i", Action::Keep, "i"},
    {"iframe", Action::Drop, nullptr},
    {"img", Action::Drop, nullptr},
    {"input", Action::Drop, nullptr},
    {"ins", Action::Keep, "u"},
    {"label", Action::Unwrap, nullptr},
    {"li", Action::Keep, "li"},
    {"link", Action::Drop, nullptr},
    {"main", Action::Keep, "div"},
    {"map", Action::Drop, nullptr},
    {"math", Action::Drop, nullptr},  // replaced by its alttext, see below
    {"meta", Action::Drop, nullptr},
    {"nav", Action::Drop, nullptr},
    {"noscript", Action::Drop, nullptr},
    {"object", Action::Drop, nullptr},
    {"ol", Action::Keep, "ol"},
    {"p", Action::Keep, "p"},
    {"picture", Action::Drop, nullptr},
    {"pre", Action::Keep, "div"},
    {"rp", Action::Drop, nullptr},
    {"rt", Action::Keep, "rt"},
    {"ruby", Action::Keep, "ruby"},
    {"s", Action::Keep, "s"},
    {"script", Action::Drop, nullptr},
    {"section", Action::Keep, "div"},
    {"select", Action::Drop, nullptr},
    {"source", Action::Drop, nullptr},
    {"strike", Action::Keep, "s"},
    {"strong", Action::Keep, "b"},
    {"style", Action::Drop, nullptr},
    {"sub", Action::Keep, "sub"},
    {"summary", Action::Keep, "div"},
    {"sup", Action::Keep, "sup"},
    {"svg", Action::Drop, nullptr},
    {"table", Action::Keep, "table"},
    {"td", Action::Keep, "td"},
    {"template", Action::Drop, nullptr},
    {"textarea", Action::Drop, nullptr},
    {"th", Action::Keep, "th"},
    {"title", Action::Drop, nullptr},
    {"tr", Action::Keep, "tr"},
    {"track", Action::Drop, nullptr},
    {"u", Action::Keep, "u"},
    {"ul", Action::Keep, "ul"},
    {"var", Action::Keep, "i"},
    {"video", Action::Drop, nullptr},
    {"wbr", Action::Drop, nullptr},
};

const TagRule* findRule(std::string_view name) {
  size_t lo = 0, hi = std::size(kRules);
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    const int c = name.compare(kRules[mid].name);
    if (c == 0) return &kRules[mid];
    if (c < 0)
      hi = mid;
    else
      lo = mid + 1;
  }
  return nullptr;
}

bool isVoid(std::string_view n) {
  static constexpr const char* kVoid[] = {"area",  "base", "br",   "col",   "embed",  "hr",    "img",
                                          "input", "link", "meta", "param", "source", "track", "wbr"};
  return std::any_of(std::begin(kVoid), std::end(kVoid), [&](const char* v) { return n == v; });
}

// Elements whose content is raw text (no tags inside) in HTML.
bool isRawText(std::string_view n) { return n == "script" || n == "style" || n == "textarea" || n == "title"; }

// A start tag of one of these implicitly closes an open <p>.
bool closesParagraph(std::string_view n) {
  static constexpr const char* kTags[] = {"address", "article", "aside",  "blockquote", "details", "dd",    "div",
                                          "dl",      "dt",      "figure", "footer",     "h1",      "h2",    "h3",
                                          "h4",      "h5",      "h6",     "header",     "hr",      "li",    "main",
                                          "nav",     "ol",      "p",      "pre",        "section", "table", "ul"};
  return std::any_of(std::begin(kTags), std::end(kTags), [&](const char* v) { return n == v; });
}

// Class names (whole tokens) whose elements are dropped with their content.
// MediaWiki and mwoffliner chrome, citation markers and image frames.
bool droppedClassToken(std::string_view t) {
  static constexpr const char* kExact[] = {
      "ambox",
      "catlinks",
      "gallery",
      "hatnote",
      "infobox-image",
      "metadata",
      "mbox-small",
      "mw-cite-backlink",
      "mw-editsection",
      "mw-empty-elt",
      "mw-jump-link",
      "mw-ref",
      "mw-references-wrap",
      "navbox",
      "navbox-styles",
      "navigation-not-searchable",
      "noprint",
      "portalbox",
      "printfooter",
      "refbegin",
      "reference",
      "references",
      "reflist",
      "shortdescription",
      "side-box",
      "sistersitebox",
      "thumb",
      "tmulti",
      "toc",
      "vertical-navbox",
      "mwe-math-fallback-image-inline",
      "mwe-math-fallback-image-display",
      "mw-kartographer-maplink",
      "noviewer",
      "plainlinks-print",
      "mw-authority-control",
  };
  return std::any_of(std::begin(kExact), std::end(kExact), [&](const char* v) { return t == v; });
}

bool classDropped(std::string_view cls) {
  size_t i = 0;
  while (i < cls.size()) {
    while (i < cls.size() && isSpace(cls[i])) i++;
    size_t j = i;
    while (j < cls.size() && !isSpace(cls[j])) j++;
    if (j > i && droppedClassToken(cls.substr(i, j - i))) return true;
    i = j;
  }
  return false;
}

bool styleHidden(std::string_view style) {
  // "display:none" with any spacing/case.
  std::string compact;
  compact.reserve(style.size());
  for (char c : style)
    if (!isSpace(c)) compact.push_back(lower(c));
  return compact.find("display:none") != std::string::npos;
}

bool isExternalHref(std::string_view href) {
  auto starts = [&](const char* p) { return href.substr(0, strlen(p)) == p; };
  return starts("http:") || starts("https:") || starts("//") || starts("mailto:") || starts("ftp:") || starts("tel:") ||
         starts("javascript:") || starts("geo:");
}

// ---------------------------------------------------------------- cleaner

struct Tag {
  std::string name;                                             // lower-case
  std::vector<std::pair<std::string, std::string_view>> attrs;  // lower-case name, raw value
  bool selfClosing = false;
  bool complete = false;  // the closing '>' was found

  std::string_view attr(std::string_view n) const {
    for (const auto& a : attrs)
      if (a.first == n) return a.second;
    return {};
  }
  bool hasAttr(std::string_view n) const {
    for (const auto& a : attrs)
      if (a.first == n) return true;
    return false;
  }
};

class Cleaner {
 public:
  Cleaner(const HtmlCleanOptions& options, HtmlSink& sink, HtmlCleanStats& stats)
      : options_(options), sink_(sink), stats_(stats) {
    out_.reserve(kFlushAt + 512);
  }

  bool run(std::string_view html) {
    stats_.inputBytes = html.size();
    out_ += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>";
    appendText(out_, options_.title);
    out_ += "</title></head><body>\n";

    size_t i = 0;
    const size_t n = html.size();
    while (i < n && ok_) {
      if (html[i] != '<') {
        size_t j = html.find('<', i);
        if (j == std::string_view::npos) j = n;
        text(html.substr(i, j - i));
        i = j;
        continue;
      }
      // Comment.
      if (html.compare(i, 4, "<!--") == 0) {
        const size_t end = html.find("-->", i + 4);
        i = (end == std::string_view::npos) ? n : end + 3;
        continue;
      }
      // Doctype, CDATA, processing instruction: skip to '>'.
      if (i + 1 < n && (html[i + 1] == '!' || html[i + 1] == '?')) {
        const size_t end = html.find('>', i + 2);
        i = (end == std::string_view::npos) ? n : end + 1;
        continue;
      }
      if (i + 1 < n && html[i + 1] == '/') {
        size_t j = i + 2;
        std::string name;
        while (j < n && !isSpace(html[j]) && html[j] != '>' && html[j] != '/') name.push_back(lower(html[j++]));
        const size_t end = html.find('>', j);
        i = (end == std::string_view::npos) ? n : end + 1;
        if (!name.empty()) endTag(name);
        continue;
      }
      if (i + 1 < n && isAsciiAlpha(html[i + 1])) {
        Tag tag;
        i = parseTag(html, i + 1, tag);
        if (!tag.complete) break;  // input ends inside a tag: ignore the fragment, as browsers do
        if (isRawText(tag.name) && !tag.selfClosing) {
          // Skip to the matching close tag without looking inside.
          const size_t close = findRawTextEnd(html, i, tag.name);
          const size_t gt = (close == std::string_view::npos) ? n : html.find('>', close);
          i = (gt == std::string_view::npos) ? n : gt + 1;
          if (dropDepth_ == 0) stats_.subtreesDropped++;
          continue;
        }
        startTag(tag);
        continue;
      }
      // A '<' that starts nothing: literal text.
      text(html.substr(i, 1));
      i++;
    }

    while (!stack_.empty()) pop();
    out_ += "\n</body></html>\n";
    flush(true);
    return ok_;
  }

 private:
  struct Open {
    std::string name;  // input tag name
    const char* out;   // emitted output tag, or nullptr
  };

  static constexpr size_t kFlushAt = 4096;

  size_t parseTag(std::string_view html, size_t i, Tag& tag) {
    const size_t n = html.size();
    while (i < n && !isSpace(html[i]) && html[i] != '>' && html[i] != '/') tag.name.push_back(lower(html[i++]));
    for (;;) {
      while (i < n && (isSpace(html[i]))) i++;
      if (i >= n) return n;
      if (html[i] == '>') {
        tag.complete = true;
        return i + 1;
      }
      if (html[i] == '/') {
        if (i + 1 < n && html[i + 1] == '>') {
          tag.selfClosing = true;
          tag.complete = true;
          return i + 2;
        }
        i++;
        continue;
      }
      std::string an;
      while (i < n && !isSpace(html[i]) && html[i] != '=' && html[i] != '>' &&
             !(html[i] == '/' && i + 1 < n && html[i + 1] == '>'))
        an.push_back(lower(html[i++]));
      while (i < n && isSpace(html[i])) i++;
      std::string_view value;
      if (i < n && html[i] == '=') {
        i++;
        while (i < n && isSpace(html[i])) i++;
        if (i < n && (html[i] == '"' || html[i] == '\'')) {
          const char q = html[i++];
          const size_t end = html.find(q, i);
          const size_t stop = (end == std::string_view::npos) ? n : end;
          value = html.substr(i, stop - i);
          i = (end == std::string_view::npos) ? n : end + 1;
        } else {
          const size_t start = i;
          while (i < n && !isSpace(html[i]) && html[i] != '>') i++;
          value = html.substr(start, i - start);
        }
      }
      if (!an.empty() && tag.attrs.size() < 32) tag.attrs.emplace_back(std::move(an), value);
    }
  }

  static size_t findRawTextEnd(std::string_view html, size_t from, const std::string& name) {
    for (size_t k = html.find("</", from); k != std::string_view::npos; k = html.find("</", k + 2)) {
      size_t m = 0;
      while (m < name.size() && k + 2 + m < html.size() && lower(html[k + 2 + m]) == name[m]) m++;
      if (m == name.size()) return k;
    }
    return std::string_view::npos;
  }

  void startTag(const Tag& tag) {
    const std::string& name = tag.name;
    const bool isVoidTag = isVoid(name) || tag.selfClosing;

    if (dropDepth_ > 0) {
      if (!isVoidTag) {
        stack_.push_back({name, nullptr});
      }
      return;
    }

    // HTML's implied end tags, enough for MediaWiki output.
    if (closesParagraph(name)) closeOpen("p", {"div", "li", "td", "th", "blockquote", "table"});
    if (name == "li") closeOpen("li", {"ul", "ol", "table"});
    if (name == "dt" || name == "dd") {
      closeOpen("dt", {"dl", "table"});
      closeOpen("dd", {"dl", "table"});
    }
    if (name == "tr") closeOpen("tr", {"table"});
    if (name == "td" || name == "th" || name == "tr") {
      closeOpen("td", {"tr", "table"});
      closeOpen("th", {"tr", "table"});
    }

    const TagRule* rule = findRule(name);
    Action action = rule ? rule->action : Action::Unwrap;
    if (name == "html" || name == "body") action = Action::Unwrap;
    if (action != Action::Drop) {
      if (classDropped(tag.attr("class")) || styleHidden(tag.attr("style")) || tag.attr("id") == "toc" ||
          tag.attr("role") == "navigation") {
        action = Action::Drop;
      }
    }

    if (options_.images != HtmlImages::None && options_.imageList) {
      if (name == "img") {
        keepImage(tag);
        return;  // void: nothing to push
      }
      // Picture frames (infobox image cells, figures, thumbnails) stay while
      // pictures are wanted, so their image and caption come through.
      if (wantImages() && !styleHidden(tag.attr("style")) && isPictureFrame(name, tag.attr("class"))) {
        action = Action::Unwrap;
      }
    }

    if (name == "math") {
      // Kiwix keeps the TeX source; show it instead of a missing image.
      mathAltText(tag.attr("alttext"));
    }

    if (action == Action::Drop) {
      stats_.subtreesDropped++;
      if (!isVoidTag) {
        stack_.push_back({name, nullptr});
        dropDepth_ = stack_.size();
      }
      return;
    }

    const char* outTag = nullptr;
    if (action == Action::Keep) {
      outTag = rule->out;
      if (name == "a") {
        const std::string_view href = tag.attr("href");
        if (!options_.keepLinks || href.empty() || isExternalHref(href) || href[0] == '#') outTag = nullptr;
      }
    }

    // Table of contents: a heading opens a record; ids on it or inside it
    // become its aliases.
    bool opensHeading = false;
    if (options_.headings && dropDepth_ == 0) {
      if (headingDepth_ == 0 && outTag && isHeadingTag(outTag) && !isVoidTag) {
        HtmlHeading h;
        h.level = static_cast<uint8_t>(outTag[1] - '0');
        options_.headings->push_back(std::move(h));
        opensHeading = true;
      }
      if ((opensHeading || headingDepth_ > 0) && tag.hasAttr("id")) {
        auto& aliases = options_.headings->back().aliases;
        if (aliases.size() < 4) aliases.push_back(plainText(tag.attr("id")));
      }
    }

    if (outTag) {
      stats_.elementsKept++;
      out_ += '<';
      out_ += outTag;
      writeAttrs(tag, outTag);
      if (isVoidTag) {
        out_ += "/>";
        if (strcmp(outTag, "br") == 0) out_ += '\n';
      } else {
        out_ += '>';
      }
    }
    if (!isVoidTag) stack_.push_back({name, outTag});
    if (opensHeading) headingDepth_ = stack_.size();
    // The lead ends at the first section heading.
    if (outTag && isHeadingTag(outTag) && outTag[1] != '1') {
      leadDone_ = true;
      sawSection_ = true;
    }
    flush(false);
  }

  void writeAttrs(const Tag& tag, const char* outTag) {
    auto emit = [&](const char* name, std::string_view raw) {
      out_ += ' ';
      out_ += name;
      out_ += "=\"";
      appendText(out_, raw);
      out_ += '"';
    };
    const bool heading = isHeadingTag(outTag);
    if (heading && options_.headings && headingDepth_ == 0 && dropDepth_ == 0) {
      out_ += " id=\"";
      out_ += headingAnchor(options_.headings->size() - 1);
      out_ += '"';
    } else if (heading && tag.hasAttr("id")) {
      emit("id", tag.attr("id"));
    }
    if (tag.hasAttr("dir")) {
      const std::string_view d = tag.attr("dir");
      if (d == "rtl" || d == "ltr") emit("dir", d);
    }
    if (strcmp(outTag, "td") == 0 || strcmp(outTag, "th") == 0) {
      if (tag.hasAttr("colspan")) emit("colspan", tag.attr("colspan"));
      if (tag.hasAttr("rowspan")) emit("rowspan", tag.attr("rowspan"));
    }
    if (strcmp(outTag, "a") == 0) emit("href", tag.attr("href"));
  }

  void mathAltText(std::string_view alt) {
    if (alt.empty() || dropDepth_ > 0) return;
    // "{\displaystyle x^{2}}" -> "x^{2}"
    std::string_view a = alt;
    const std::string_view prefix = "{\\displaystyle";
    if (a.substr(0, prefix.size()) == prefix && a.back() == '}') {
      a = a.substr(prefix.size(), a.size() - prefix.size() - 1);
      while (!a.empty() && isSpace(a.front())) a.remove_prefix(1);
    }
    out_ += "<i>";
    appendText(out_, a);
    out_ += "</i>";
  }

  // Closes an open `name` element if one is open above the nearest boundary.
  void closeOpen(const char* name, std::initializer_list<const char*> boundaries) {
    for (size_t k = stack_.size(); k-- > 0;) {
      const std::string& open = stack_[k].name;
      if (open == name) {
        while (stack_.size() > k) pop();
        return;
      }
      for (const char* b : boundaries)
        if (open == b) return;
    }
  }

  void endTag(const std::string& name) {
    for (size_t k = stack_.size(); k-- > 0;) {
      if (stack_[k].name == name) {
        while (stack_.size() > k) pop();
        return;
      }
      // Don't let a stray </p> or </li> close an enclosing table or list.
      if (dropDepth_ == 0 && (stack_[k].name == "table" || stack_[k].name == "body")) return;
    }
    // A </p> with no open <p> is an empty paragraph in HTML; ignore it.
  }

  void pop() {
    const Open& top = stack_.back();
    if (top.out) {
      out_ += "</";
      out_ += top.out;
      out_ += '>';
      if (strcmp(top.out, "p") == 0 || top.out[0] == 'h' || strcmp(top.out, "li") == 0 || strcmp(top.out, "div") == 0 ||
          strcmp(top.out, "tr") == 0)
        out_ += '\n';
    }
    // A section's opening text: paragraphs and list items joined by a space.
    if (top.out && (strcmp(top.out, "p") == 0 || strcmp(top.out, "li") == 0) && options_.headings &&
        !options_.headings->empty() && !options_.headings->back().summary.empty()) {
      options_.headings->back().summary += ' ';
    }
    // Paragraphs of the lead are joined by a space; one long enough is the lead.
    if (top.out && strcmp(top.out, "p") == 0 && options_.lead && !leadDone_ && !options_.lead->empty()) {
      *options_.lead += ' ';
      if (options_.lead->size() >= 280) leadDone_ = true;
    }
    stack_.pop_back();
    if (dropDepth_ > stack_.size()) dropDepth_ = 0;
    if (headingDepth_ > stack_.size()) {
      headingDepth_ = 0;
      collapseSpaces(options_.headings->back().text);
    }
    flush(false);
  }

  void text(std::string_view t) {
    if (dropDepth_ > 0 || t.empty()) return;
    if (headingDepth_ > 0) {
      std::string& h = options_.headings->back().text;
      if (h.size() < 200) appendText(h, t, false);
    }
    if (options_.headings && !options_.headings->empty() && headingDepth_ == 0) {
      std::string& summary = options_.headings->back().summary;
      if (summary.size() < kSummaryCollect && inSectionProse()) appendText(summary, t, false);
    }
    if (options_.lead && !leadDone_ && inLeadParagraph()) {
      std::string& lead = *options_.lead;
      appendText(lead, t, false);
      if (lead.size() >= options_.leadLimit) leadDone_ = true;
    }
    appendText(out_, t);
    flush(false);
  }

  bool wantImages() const {
    return options_.images == HtmlImages::All ||
           (options_.images == HtmlImages::Lead && !leadImageTaken_ && !sawSection_);
  }

  static bool isPictureFrame(const std::string& name, std::string_view cls) {
    if (name == "figure") return true;
    size_t i = 0;
    while (i < cls.size()) {
      while (i < cls.size() && isSpace(cls[i])) i++;
      size_t j = i;
      while (j < cls.size() && !isSpace(cls[j])) j++;
      const std::string_view t = cls.substr(i, j - i);
      if (t == "infobox-image" || t == "thumb" || t == "tmulti" || t == "thumbinner") return true;
      i = j;
    }
    return false;
  }

  // The picture format an <img src> names, by its extension: WebP (Kiwix's
  // Wikipedia), JPEG or PNG (other collections, e.g. Wikivoyage). Anything
  // else (SVG, GIF) is not one the reader can show.
  static ImageFormat imageFormat(std::string_view src) {
    const size_t cut = src.find_first_of("?#");
    if (cut != std::string_view::npos) src = src.substr(0, cut);
    const size_t dot = src.rfind('.');
    if (dot == std::string_view::npos) return ImageFormat::Unknown;
    std::string ext(src.substr(dot + 1));
    for (char& c : ext) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (ext == "webp") return ImageFormat::WebP;
    if (ext == "png") return ImageFormat::Png;
    if (ext == "jpg" || ext == "jpeg") return ImageFormat::Jpeg;
    return ImageFormat::Unknown;
  }

  // An <img> while pictures are wanted: a WebP, JPEG or PNG of a useful size
  // becomes <img src="/pl-img/N.png" width height> (N.jpg for a JPEG, which
  // the reader shows as it is); anything else is dropped.
  void keepImage(const Tag& tag) {
    if (!wantImages() || classDropped(tag.attr("class"))) return;
    const std::string_view src = tag.attr("src");
    int w = atoi(std::string(tag.attr("width")).c_str());
    int h = atoi(std::string(tag.attr("height")).c_str());
    if (w <= 0 || h <= 0) {
      // No size on the tag: the original's, which has the same shape.
      w = atoi(std::string(tag.attr("data-file-width")).c_str());
      h = atoi(std::string(tag.attr("data-file-height")).c_str());
    }
    const ImageFormat format = imageFormat(src);
    if (format == ImageFormat::Unknown || w < 60 || h < 40) return;
    const size_t n = options_.imageList->size();
    options_.imageList->push_back({std::string(src), w, h, format});
    if (options_.images == HtmlImages::Lead) leadImageTaken_ = true;
    out_ += "<img src=\"";
    out_ += kArticleImagePrefix;
    out_ += std::to_string(n);
    out_ += format == ImageFormat::Jpeg ? ".jpg" : ".png";
    out_ += "\" width=\"" + std::to_string(w) + "\" height=\"" + std::to_string(h) + "\"";
    if (tag.hasAttr("alt")) {
      out_ += " alt=\"";
      appendText(out_, tag.attr("alt"));
      out_ += '"';
    }
    out_ += "/>";
    stats_.elementsKept++;
    flush(false);
  }

  // Text of a <p> or <li> outside tables and headings: what a section opens
  // with, for its line in the contents.
  bool inSectionProse() const {
    bool prose = false;
    for (const Open& o : stack_) {
      if (!o.out) continue;
      if (strcmp(o.out, "p") == 0 || strcmp(o.out, "li") == 0) prose = true;
      if (strcmp(o.out, "td") == 0 || strcmp(o.out, "th") == 0 || strcmp(o.out, "table") == 0 || isHeadingTag(o.out))
        return false;
    }
    return prose;
  }
  static constexpr size_t kSummaryCollect = 400;

  // Text of a <p> that isn't inside a table, list or heading: body prose.
  bool inLeadParagraph() const {
    bool para = false;
    for (const Open& o : stack_) {
      if (!o.out) continue;
      if (strcmp(o.out, "p") == 0) para = true;
      if (strcmp(o.out, "td") == 0 || strcmp(o.out, "th") == 0 || strcmp(o.out, "li") == 0 ||
          strcmp(o.out, "table") == 0 || isHeadingTag(o.out) || strcmp(o.out, "blockquote") == 0)
        return false;
    }
    return para;
  }

  void flush(bool force) {
    if (!ok_) return;
    if (!force && out_.size() < kFlushAt) return;
    if (!out_.empty()) {
      ok_ = sink_.write(out_.data(), out_.size());
      stats_.outputBytes += out_.size();
      out_.clear();
    }
  }

  const HtmlCleanOptions& options_;
  HtmlSink& sink_;
  HtmlCleanStats& stats_;
  std::string out_;
  std::vector<Open> stack_;
  size_t dropDepth_ = 0;
  bool leadDone_ = false;
  bool sawSection_ = false;
  bool leadImageTaken_ = false;
  size_t headingDepth_ = 0;  // stack size at which the open heading began; 0 = none

  static void collapseSpaces(std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool space = true;  // drops leading spaces
    for (char c : s) {
      if (isSpace(c)) {
        if (!space) out.push_back(' ');
        space = true;
      } else {
        out.push_back(c);
        space = false;
      }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    s.swap(out);
  }  // stack size at which the dropped subtree began; 0 = not dropping
  bool ok_ = true;
};

}  // namespace

std::string headingAnchor(size_t index) { return "pl-h" + std::to_string(index); }

uint32_t decodeHtmlEntity(std::string_view name) {
  if (name.empty()) return 0;
  if (name[0] == '#') {
    uint32_t cp = 0;
    size_t i = 1;
    const bool hex = name.size() > 1 && (name[1] == 'x' || name[1] == 'X');
    if (hex) i = 2;
    if (i >= name.size()) return 0;
    for (; i < name.size(); i++) {
      const char c = name[i];
      uint32_t d;
      if (isAsciiDigit(c))
        d = static_cast<uint32_t>(c - '0');
      else if (hex && c >= 'a' && c <= 'f')
        d = static_cast<uint32_t>(c - 'a' + 10);
      else if (hex && c >= 'A' && c <= 'F')
        d = static_cast<uint32_t>(c - 'A' + 10);
      else
        return 0;
      cp = cp * (hex ? 16 : 10) + d;
      if (cp > 0x10FFFF) return 0xFFFD;
    }
    // Windows-1252 numeric references, as browsers interpret them.
    if (cp >= 0x80 && cp <= 0x9F) {
      static constexpr uint16_t k1252[32] = {0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                                             0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD,
                                             0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                                             0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178};
      cp = k1252[cp - 0x80];
    }
    if (cp == 0) return 0xFFFD;
    return cp;
  }
  size_t lo = 0, hi = std::size(kEntities);
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    const int c = name.compare(kEntities[mid].name);
    if (c == 0) return kEntities[mid].cp;
    if (c < 0)
      hi = mid;
    else
      lo = mid + 1;
  }
  return 0;
}

namespace {
// Whitespace runs -> one space, trimmed; then at most `limit` bytes, cut at a
// word (or a UTF-8 boundary) with "…".
void tidyLead(std::string& s, size_t limit) {
  std::string out;
  out.reserve(s.size());
  bool space = false;
  for (const char c : s) {
    if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
      space = !out.empty();
      continue;
    }
    if (space) out += ' ';
    space = false;
    out += c;
  }
  if (out.size() > limit) {
    size_t cut = out.rfind(' ', limit);
    if (cut == std::string::npos || cut < limit / 2) {
      cut = limit;
      while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) cut--;
    }
    out.resize(cut);
    while (!out.empty() && (out.back() == ',' || out.back() == ';' || out.back() == ':')) out.pop_back();
    out += "\xE2\x80\xA6";
  }
  s = std::move(out);
}
}  // namespace

std::string firstSentences(std::string_view text, size_t maxSentences, size_t maxBytes) {
  size_t end = 0;
  size_t found = 0;
  for (size_t i = 0; i + 2 < text.size() && found < maxSentences; i++) {
    const char c = text[i];
    if ((c == '.' || c == '!' || c == '?') && text[i + 1] == ' ' &&
        (std::isupper(static_cast<unsigned char>(text[i + 2])) || static_cast<unsigned char>(text[i + 2]) >= 0x80)) {
      // "U.S. " and "c. 1500": a one-letter word before the stop is an abbreviation.
      const bool initial = i < 2 || text[i - 2] == ' ' || text[i - 2] == '.';
      if (initial) continue;
      end = i + 1;
      found++;
    }
  }
  std::string out(found >= maxSentences && end > 0 ? text.substr(0, end) : text);
  tidyLead(out, maxBytes);
  return out;
}

bool cleanArticleHtml(std::string_view html, const HtmlCleanOptions& options, HtmlSink& sink, HtmlCleanStats* stats) {
  HtmlCleanStats local;
  Cleaner cleaner(options, sink, stats ? *stats : local);
  const bool ok = cleaner.run(html);
  if (options.lead) tidyLead(*options.lead, options.leadLimit);
  if (options.headings) {
    for (HtmlHeading& h : *options.headings) {
      if (h.summary.empty()) continue;
      tidyLead(h.summary, 400);
      h.summary = firstSentences(h.summary, 1, 160);
    }
  }
  return ok;
}

}  // namespace zim
