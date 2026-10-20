#include "BookTextSearch.h"

#include <Utf8.h>
#include <strings.h>

#include <cstring>

#include "Epub/VisibleText.h"
#include "Epub/htmlEntities.h"

namespace booksearch {

namespace {

// Latin Extended-A (U+0100..U+017F) folded to its base letter. '#' marks a
// case pair kept as a letter of its own (Ĳ/ĳ, Œ/œ): even -> odd codepoint.
constexpr char LATIN_EXT_A[] =
    "aaaaaa"        // 0100-0105 Ā ā Ă ă Ą ą
    "cccccccc"      // 0106-010D
    "dddd"          // 010E-0111
    "eeeeeeeeee"    // 0112-011B
    "gggggggg"      // 011C-0123
    "hhhh"          // 0124-0127
    "iiiiiiiiii"    // 0128-0131
    "##"            // 0132-0133 Ĳ ĳ
    "jj"            // 0134-0135
    "kkk"           // 0136-0138
    "llllllllll"    // 0139-0142
    "nnnnnnnnn"     // 0143-014B
    "oooooo"        // 014C-0151
    "##"            // 0152-0153 Œ œ
    "rrrrrr"        // 0154-0159
    "ssssssss"      // 015A-0161
    "tttttt"        // 0162-0167
    "uuuuuuuuuuuu"  // 0168-0173
    "ww"            // 0174-0175
    "yyy"           // 0176-0178
    "zzzzzz"        // 0179-017E
    "s";            // 017F ſ
static_assert(sizeof(LATIN_EXT_A) - 1 == 0x80, "Latin Extended-A table must cover U+0100..U+017F");

// Latin-1 Supplement letters U+00C0..U+00FF; 0 = keep as is (lowercased).
constexpr char LATIN_1[] =
    "aaaaaa"  // C0-C5 À-Å
    "\0"      // C6 Æ
    "c"       // C7 Ç
    "eeee"    // C8-CB
    "iiii"    // CC-CF
    "dn"      // D0 Ð, D1 Ñ
    "ooooo"   // D2-D6
    "\0"      // D7 ×
    "o"       // D8 Ø
    "uuuu"    // D9-DC
    "y"       // DD Ý
    "\0\0"    // DE Þ, DF ß
    "aaaaaa"  // E0-E5
    "\0"      // E6 æ
    "c"       // E7
    "eeee"    // E8-EB
    "iiii"    // EC-EF
    "dn"      // F0 ð, F1 ñ
    "ooooo"   // F2-F6
    "\0"      // F7 ÷
    "o"       // F8 ø
    "uuuu"    // F9-FC
    "y\0y";   // FD ý, FE þ, FF ÿ
static_assert(sizeof(LATIN_1) - 1 == 0x40, "Latin-1 table must cover U+00C0..U+00FF");

bool isBlockElement(const char* name) {
  static constexpr const char* BLOCKS[] = {
      "p",          "div",     "br",    "li",     "h1",         "h2",     "h3",     "h4",    "h5",     "h6",
      "tr",         "td",      "th",    "dt",     "dd",         "pre",    "hr",     "table", "ul",     "ol",
      "blockquote", "section", "aside", "figure", "figcaption", "header", "footer", "nav",   "article"};
  for (const char* tag : BLOCKS) {
    if (strcasecmp(name, tag) == 0) return true;
  }
  return false;
}

const char* stripPrefix(const char* name) {
  const char* colon = strrchr(name, ':');
  return colon ? colon + 1 : name;
}

}  // namespace

uint32_t foldCodepoint(const uint32_t cp) {
  if (cp < 0x80) {
    if (cp >= 'A' && cp <= 'Z') return cp + 32;
    if (cp == '\t' || cp == '\n' || cp == '\r' || cp == '\f' || cp == '\v') return ' ';
    return cp;
  }
  if (cp == 0x00A0 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x202F || cp == 0x205F || cp == 0x3000) return ' ';
  if (cp == 0x00AD || (cp >= 0x200B && cp <= 0x200D) || cp == 0x2060 || cp == 0xFEFF) return 0;
  if (cp >= 0x00C0 && cp <= 0x00FF) {
    const char base = LATIN_1[cp - 0x00C0];
    if (base) return static_cast<uint32_t>(base);
    if (cp == 0x00C6 || cp == 0x00DE) return cp + 0x20;  // Æ -> æ, Þ -> þ
    return cp;
  }
  if (cp >= 0x0100 && cp <= 0x017F) {
    const char base = LATIN_EXT_A[cp - 0x0100];
    if (base == '#') return cp | 1u;
    return static_cast<uint32_t>(base);
  }
  if (cp >= 0x0391 && cp <= 0x03A9 && cp != 0x03A2) return cp + 0x20;  // Greek capitals
  if (cp >= 0x0410 && cp <= 0x042F) return cp + 0x20;                  // Cyrillic А-Я
  if (cp >= 0x0400 && cp <= 0x040F) return cp + 0x50;                  // Cyrillic Ѐ-Џ
  switch (cp) {
    case 0x2018:  // ‘
    case 0x2019:  // ’
    case 0x201B:  // ‛
    case 0x2032:  // ′
      return '\'';
    case 0x201C:  // “
    case 0x201D:  // ”
    case 0x201E:  // „
    case 0x2033:  // ″
      return '"';
    case 0x2010:  // ‐
    case 0x2011:  // non-breaking hyphen
    case 0x2012:  // ‒
    case 0x2013:  // –
    case 0x2014:  // —
    case 0x2015:  // ―
      return '-';
    default:
      return cp;
  }
}

std::vector<uint32_t> foldQuery(const std::string& utf8) {
  std::vector<uint32_t> out;
  const auto* p = reinterpret_cast<const unsigned char*>(utf8.c_str());
  bool lastSpace = true;
  while (*p) {
    const uint32_t folded = foldCodepoint(utf8NextCodepoint(&p));
    if (folded == 0) continue;
    if (folded == ' ') {
      if (!lastSpace) out.push_back(' ');
      lastSpace = true;
      continue;
    }
    out.push_back(folded);
    lastSpace = false;
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

ChapterSearcher::ChapterSearcher(const std::vector<uint32_t>& needleIn, const size_t maxHitsIn,
                                 const size_t contextCharsIn)
    : needle(needleIn), contextChars(contextCharsIn), maxHits(maxHitsIn) {
  if (needle.empty() || maxHits == 0) return;
  // KMP failure table.
  failure.assign(needle.size(), 0);
  for (size_t i = 1, k = 0; i < needle.size(); ++i) {
    while (k > 0 && needle[i] != needle[k]) k = failure[k - 1];
    if (needle[i] == needle[k]) ++k;
    failure[i] = static_cast<int>(k);
  }
  offsetRing.assign(needle.size(), 0);
  recent.assign(contextChars + needle.size(), 0);
  // Same parse as the layout parser (HTML void-tag repair, entity default handler), but the
  // Lean profile: search never reads attributes, so the ~5 KB attribute table is not allocated.
  parserOk = parser.init(this, &ChapterSearcher::startElement, &ChapterSearcher::endElement,
                         &ChapterSearcher::characterData, &ChapterSearcher::defaultHandler,
                         /*htmlVoidTagRepair=*/true, SaxParser::Profile::Lean);
}

ChapterSearcher::~ChapterSearcher() = default;  // SaxParser frees its state

void ChapterSearcher::stopParsing() {
  stopped = true;
  parser.stop();
}

size_t ChapterSearcher::write(const uint8_t* buffer, const size_t size) {
  if (!parserOk || parseFailed || stopped) return size;  // the zip reader polls stopFlag()
  if (abortProbe && abortProbe()) {
    wasAborted = true;
    stopParsing();
    return size;
  }
  if (!parser.feed(buffer, size) && !parser.isStopped()) {
    parseFailed = true;
    stopped = true;
  }
  return size;
}

bool ChapterSearcher::finish() {
  if (parserOk && !parseFailed && !stopped) {
    if (!parser.finalize()) parseFailed = true;
  }
  pending.clear();  // chapter end: snippets keep what they have
  // A parse error part-way still leaves valid hits from the text before it.
  return !parseFailed || !found.empty();
}

void ChapterSearcher::startElement(void* userData, const char* name, const char**) {
  static_cast<ChapterSearcher*>(userData)->onStart(name);
}

void ChapterSearcher::endElement(void* userData, const char* name) {
  static_cast<ChapterSearcher*>(userData)->onEnd(name);
}

void ChapterSearcher::characterData(void* userData, const char* s, const int len) {
  static_cast<ChapterSearcher*>(userData)->onText(s, len);
}

void ChapterSearcher::defaultHandler(void* userData, const char* s, const int len) {
  // Mirrors ChapterHtmlSlimParser::defaultHandlerExpand: a known entity counts as its
  // expansion, an unknown one as its literal "&name;" text.
  if (len >= 3 && s[0] == '&' && s[len - 1] == ';') {
    auto* self = static_cast<ChapterSearcher*>(userData);
    const char* value = lookupHtmlEntity(s, static_cast<size_t>(len));
    if (value) {
      self->onText(value, static_cast<int>(strlen(value)));
    } else {
      self->onText(s, len);
    }
  }
}

// Body and hidden-tag tracking as in ChapterHtmlSlimParser: <body> (any case) switches
// counting on for the rest of the document; head/script/style nest a hidden depth anywhere.
void ChapterSearcher::onStart(const char* rawName) {
  const char* name = stripPrefix(rawName);
  if (!insideBody && strcasecmp(name, "body") == 0) insideBody = true;
  if (VisibleText::isNonVisibleTag(name)) nonVisibleDepth++;
  if (insideBody && nonVisibleDepth == 0 && isBlockElement(name)) blockBoundary();
}

void ChapterSearcher::onEnd(const char* rawName) {
  const char* name = stripPrefix(rawName);
  if (VisibleText::isNonVisibleTag(name) && nonVisibleDepth > 0) {
    nonVisibleDepth--;
    return;
  }
  if (insideBody && nonVisibleDepth == 0 && isBlockElement(name)) blockBoundary();
}

void ChapterSearcher::onText(const char* s, const int len) {
  if (!insideBody || nonVisibleDepth > 0 || len <= 0 || stopped) return;
  const auto* p = reinterpret_cast<const unsigned char*>(s);
  const auto* end = p + len;
  while (p < end && !stopped) {
    const auto* start = p;
    const uint32_t original = utf8NextCodepoint(&p);
    // Offsets count non-whitespace bytes (VisibleText::visibleBytes): a whitespace byte adds
    // nothing, any other codepoint its UTF-8 length.
    const uint32_t offset = visibleOffset;
    if (!(p - start == 1 && VisibleText::isSpace(*start))) visibleOffset += static_cast<uint32_t>(p - start);
    const uint32_t folded = foldCodepoint(original);
    if (folded == 0) continue;
    if (folded == ' ') {
      if (lastWasSpace) continue;
      lastWasSpace = true;
      emit(' ', ' ', offset, visibleOffset);
      continue;
    }
    lastWasSpace = false;
    emit(folded, original, offset, visibleOffset);
  }
}

void ChapterSearcher::blockBoundary() {
  if (lastWasSpace) return;
  lastWasSpace = true;
  emit(' ', ' ', visibleOffset, visibleOffset);  // virtual space: no source bytes
}

void ChapterSearcher::appendContext(const uint32_t original) {
  for (size_t i = 0; i < pending.size();) {
    Pending& p = pending[i];
    utf8AppendCodepoint(found[p.hitIndex].snippet, original);
    if (--p.remaining == 0) {
      found[p.hitIndex].snippet += "\xE2\x80\xA6";  // more text follows
      pending.erase(pending.begin() + static_cast<long>(i));
    } else {
      ++i;
    }
  }
}

void ChapterSearcher::emit(const uint32_t folded, const uint32_t original, const uint32_t srcOffset,
                           const uint32_t srcEnd) {
  appendContext(original);
  if (full()) {
    // Only finishing the last snippets' context now.
    if (pending.empty()) stopParsing();
    return;
  }
  recent[recentCount % recent.size()] = original;
  recentCount++;
  offsetRing[emittedCount % offsetRing.size()] = srcOffset;
  emittedCount++;

  while (matched > 0 && folded != needle[matched]) matched = failure[matched - 1];
  if (folded == needle[matched]) matched++;
  if (matched < static_cast<int>(needle.size())) return;

  // Full match ending at this unit.
  const size_t n = needle.size();
  Hit hit;
  hit.visibleOffset = offsetRing[(emittedCount - n) % offsetRing.size()];
  hit.visibleEnd = srcEnd;
  // Snippet: up to contextChars before the match, then the match itself.
  const size_t available = recentCount < recent.size() ? recentCount : recent.size();
  const size_t before = available - n < contextChars ? available - n : contextChars;
  const bool clippedBefore = recentCount > before + n;
  if (clippedBefore) hit.snippet = "\xE2\x80\xA6";  // …
  for (size_t i = recentCount - n - before; i < recentCount; ++i) {
    const uint32_t c = recent[i % recent.size()];
    if (hit.snippet.empty() && c == ' ') continue;
    utf8AppendCodepoint(hit.snippet, c);
  }
  found.push_back(std::move(hit));
  if (contextChars > 0) pending.push_back({found.size() - 1, contextChars});
  matched = 0;  // non-overlapping: "aa" in "aaaa" is two hits, not three

  if (full() && pending.empty()) stopParsing();
}

}  // namespace booksearch
