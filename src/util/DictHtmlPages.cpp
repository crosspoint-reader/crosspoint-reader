#include "DictHtmlPages.h"

#include <Arduino.h>
#include <Epub/parsers/ChapterHtmlSlimParser.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cctype>
#include <cstring>

#include "CrossPointSettings.h"

namespace {

// Normalized XHTML staged here for the file-driven parser; truncated on each
// use, removed after the parse.
constexpr const char* TMP_HTML_PATH = "/.crosspoint/dicthtml.tmp";

// ENTRY gate: is there room to start a styled layout at all? Keeps enough
// contiguous heap for the parser's 16KB SD-font advance scratch plus
// page/layout allocations. Falling back to plain text is cheaper than entering
// a throwing allocation path under pressure.
constexpr size_t MIN_STYLED_FREE_HEAP = 40 * 1024;
constexpr size_t MIN_STYLED_MAX_ALLOC = 20 * 1024;

// RETAIN gate: is there still room to keep the pages coming? Checked per
// completed page, and necessarily much lower than the entry gate, because it
// measures a different heap: the parser is alive and holding its working set.
//
// Measured on an X4 with a 2554-byte definition: 50772 bytes free on entry,
// 29400 one page in. The parse and layout cost ~21KB while they run, all of it
// returned when the parser is destroyed. Testing the entry number here charged
// that cost against the gate deciding whether the layout may continue, so the
// styled path refused its own first page unless entry heap was around 61KB --
// which, stacked over the reader, it never is. Every HTML definition silently
// took the plain-text path.
//
// The pages actually retained are bounded by the two count caps below, so this
// only has to catch genuine exhaustion. A single page's layout was seen costing
// ~8KB between two checks, so 16KB keeps about that much in hand at the low
// point while still sitting far below any successful entry heap.
constexpr size_t MIN_STYLED_RETAIN_HEAP = 16 * 1024;
constexpr size_t MIN_STYLED_RETAIN_ALLOC = 8 * 1024;

// Bound retained layout independently of input bytes: compact markup can emit
// far more objects than its source size suggests.
constexpr size_t MAX_STYLED_PAGES = 64;
constexpr size_t MAX_STYLED_PAGE_ELEMENTS = 512;

class BufferedFileWriter {
 public:
  explicit BufferedFileWriter(HalFile& file) : file(file) {}

  bool append(const char c) { return append(&c, 1); }

  bool append(const char* data, size_t len) {
    while (len > 0) {
      const size_t available = sizeof(buffer) - used;
      const size_t chunk = std::min(available, len);
      memcpy(buffer + used, data, chunk);
      used += chunk;
      data += chunk;
      len -= chunk;
      if (used == sizeof(buffer) && !flush()) return false;
    }
    return true;
  }

  bool append(const char* text) { return append(text, strlen(text)); }

  bool flush() {
    if (used == 0) return true;
    if (file.write(buffer, used) != used) return false;
    used = 0;
    return true;
  }

 private:
  HalFile& file;
  // Fixed stack staging avoids an expansion-sized XHTML heap allocation.
  char buffer[128] = {};
  size_t used = 0;
};

// HTML void elements: legal without a closing tag in HTML, but must be
// self-closed to be well-formed XML.
bool isVoidElement(const char* name, const size_t len) {
  static constexpr const char* VOID_ELEMENTS[] = {"area",  "base", "br",   "col",   "embed",  "hr",    "img",
                                                  "input", "link", "meta", "param", "source", "track", "wbr"};
  return std::any_of(std::begin(VOID_ELEMENTS), std::end(VOID_ELEMENTS),
                     [name, len](const char* v) { return strlen(v) == len && strncmp(v, name, len) == 0; });
}

// True for a well-formed entity reference at html[pos] ('&'): &name; &#123;
// or &#x1F;. On success *end is the index of the ';'.
bool isEntityRef(const std::string& html, const size_t pos, size_t* end) {
  size_t j = pos + 1;
  const size_t n = html.size();
  if (j < n && html[j] == '#') {
    j++;
    if (j < n && (html[j] == 'x' || html[j] == 'X')) j++;
    const size_t digits = j;
    while (j < n && std::isxdigit(static_cast<unsigned char>(html[j]))) j++;
    if (j == digits) return false;
  } else {
    const size_t letters = j;
    while (j < n && std::isalnum(static_cast<unsigned char>(html[j]))) j++;
    if (j == letters) return false;
  }
  if (j >= n || html[j] != ';') return false;
  *end = j;
  return true;
}

// StarDict HTML is tag soup; expat is a strict XML parser. Produce a
// well-formed XHTML document from the fragment: wrap it in a root element,
// lowercase tag names (XML is case-sensitive and the parser matches
// lowercase), self-close void elements (<br> → <br/>), drop stray void
// closers (</br>) and <!…>/<?…> constructs, and escape '&'/'<' characters
// that are not part of markup. Structural damage this cannot repair
// (mismatched tags, unquoted attribute values) surfaces as a parse error and
// the caller falls back to the plain-text path.
bool writeNormalizedXhtml(const std::string& html, HalFile& file) {
  BufferedFileWriter out(file);
  if (!out.append("<html><body>")) return false;

  const size_t n = html.size();
  size_t i = 0;
  while (i < n) {
    const char c = html[i];
    if (c == '<' && i + 1 < n && (html[i + 1] == '!' || html[i + 1] == '?')) {
      // Comment, doctype or processing instruction: drop it entirely.
      const bool isComment = html.compare(i, 4, "<!--") == 0;
      const size_t j = isComment ? html.find("-->", i + 4) : html.find('>', i);
      i = (j == std::string::npos) ? n : j + (isComment ? 3 : 1);
      continue;
    }
    if (c == '<' && i + 1 < n && (html[i + 1] == '/' || std::isalpha(static_cast<unsigned char>(html[i + 1])))) {
      // Find the tag end, honouring quoted attribute values.
      size_t j = i + 1;
      char quote = 0;
      while (j < n) {
        const char d = html[j];
        if (quote) {
          if (d == quote) quote = 0;
        } else if (d == '"' || d == '\'') {
          quote = d;
        } else if (d == '>') {
          break;
        }
        j++;
      }
      if (j == n) {  // unterminated tag: treat the '<' as literal text
        if (!out.append("&lt;")) return false;
        i++;
        continue;
      }

      const bool closing = html[i + 1] == '/';
      const size_t nameStart = i + (closing ? 2 : 1);
      size_t nameEnd = nameStart;
      char nameBuf[16];
      size_t nameLen = 0;
      while (nameEnd < j && std::isalnum(static_cast<unsigned char>(html[nameEnd]))) {
        if (nameLen < sizeof(nameBuf) - 1) {
          nameBuf[nameLen++] = static_cast<char>(std::tolower(static_cast<unsigned char>(html[nameEnd])));
        }
        nameEnd++;
      }
      const bool isVoid = isVoidElement(nameBuf, nameLen);
      if (closing && isVoid) {  // "</br>" — no XML equivalent, drop it
        i = j + 1;
        continue;
      }
      if (!out.append('<')) return false;
      if (closing && !out.append('/')) return false;
      if (!out.append(nameBuf, nameLen)) return false;
      if (!out.append(html.data() + nameEnd, j - nameEnd)) return false;  // attributes verbatim
      if (!closing && isVoid && html[j - 1] != '/' && !out.append('/')) return false;
      if (!out.append('>')) return false;
      i = j + 1;
      continue;
    }
    if (c == '<') {  // stray '<' in text ("x < y")
      if (!out.append("&lt;")) return false;
      i++;
      continue;
    }
    if (c == '&') {
      size_t entityEnd = 0;
      if (isEntityRef(html, i, &entityEnd)) {
        if (!out.append(html.data() + i, entityEnd - i + 1)) return false;
        i = entityEnd + 1;
      } else {  // bare ampersand ("Tom & Jerry")
        if (!out.append("&amp;")) return false;
        i++;
      }
      continue;
    }
    if (!out.append(c)) return false;
    i++;
  }

  return out.append("</body></html>") && out.flush();
}

}  // namespace

bool buildDictionaryHtmlPages(GfxRenderer& renderer, const std::string& definition, const uint16_t viewportWidth,
                              const uint16_t viewportHeight, std::vector<std::unique_ptr<Page>>& pagesOut) {
  if (ESP.getFreeHeap() < MIN_STYLED_FREE_HEAP || ESP.getMaxAllocHeap() < MIN_STYLED_MAX_ALLOC) {
    LOG_ERR("DHTML", "Low heap for styled definition (%u free, %u max block)", ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    return false;
  }

  {
    HalFile tmp = Storage.open(TMP_HTML_PATH, O_WRITE | O_CREAT | O_TRUNC);
    if (!tmp) {
      LOG_ERR("DHTML", "Cannot create %s", TMP_HTML_PATH);
      return false;
    }
    if (!writeNormalizedXhtml(definition, tmp)) {
      LOG_ERR("DHTML", "Short write to %s", TMP_HTML_PATH);
      return false;
    }
  }  // destructor closes the file before the parser reopens the same path

  pagesOut.clear();
  // One fixed allocation (256 bytes on C3); pages must outlive the parser.
  pagesOut.reserve(MAX_STYLED_PAGES);

  bool ok = false;
  bool resourceLimitHit = false;
  const char* limitReason = nullptr;
  size_t retainedElements = 0;
  {
    const std::string tmpPath = TMP_HTML_PATH;  // the parser stores a reference
    // Heap-allocated as Section does — the parser object is far too large for
    // a stack local. Null epub is safe: imageRendering=2 suppresses <img>
    // handling, the only path that dereferences it.
    auto parser = makeUniqueNoThrow<ChapterHtmlSlimParser>(
        nullptr, tmpPath, renderer, SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
        SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth, viewportHeight,
        SETTINGS.hyphenationEnabled, SETTINGS.focusReadingEnabled,
        [&pagesOut, &resourceLimitHit, &retainedElements, &limitReason](std::unique_ptr<Page> page, uint16_t, uint16_t,
                                                                        uint32_t) {
          if (resourceLimitHit) return;
          const size_t pageElements = page->elements.size();
          // Name the limit that fired. The three causes mean different things --
          // the count caps say the definition is genuinely too big to hold,
          // while the heap floor says only that this moment was a bad one -- and
          // a single "exceeded the budget" message cannot tell them apart.
          if (pagesOut.size() >= MAX_STYLED_PAGES) {
            limitReason = "page count";
          } else if (pageElements > MAX_STYLED_PAGE_ELEMENTS - retainedElements) {
            limitReason = "element count";
          } else if (ESP.getFreeHeap() < MIN_STYLED_RETAIN_HEAP || ESP.getMaxAllocHeap() < MIN_STYLED_RETAIN_ALLOC) {
            limitReason = "free heap";
          }
          if (limitReason != nullptr) {
            LOG_ERR("DHTML", "Styled definition stopped on %s (pages=%u elements=%u free=%u contig=%u)", limitReason,
                    static_cast<unsigned>(pagesOut.size()), static_cast<unsigned>(retainedElements + pageElements),
                    ESP.getFreeHeap(), ESP.getMaxAllocHeap());
            resourceLimitHit = true;
            pagesOut.clear();
            return;
          }
          retainedElements += pageElements;
          pagesOut.push_back(std::move(page));
        },
        /*embeddedStyle=*/false, /*contentBase=*/"", /*imageBasePath=*/"", /*imageRendering=*/2);
    if (!parser) {
      LOG_ERR("DHTML", "OOM: ChapterHtmlSlimParser");
    } else {
      ok = parser->parseAndBuildPages();  // closes the file on both outcomes
    }
  }
  Storage.remove(TMP_HTML_PATH);

  if (resourceLimitHit) {
    LOG_ERR("DHTML", "Styled definition exceeded page heap budget");
  }
  if (!ok || resourceLimitHit || pagesOut.empty()) {
    pagesOut.clear();
    return false;
  }
  return true;
}
