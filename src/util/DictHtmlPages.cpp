#include "DictHtmlPages.h"

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
std::string normalizeToXhtml(const std::string& html) {
  std::string out;
  out.reserve(html.size() + html.size() / 8 + 32);
  out += "<html><body>";

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
        out += "&lt;";
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
      out += '<';
      if (closing) out += '/';
      out.append(nameBuf, nameLen);
      out.append(html, nameEnd, j - nameEnd);  // attributes verbatim
      if (!closing && isVoid && html[j - 1] != '/') out += '/';
      out += '>';
      i = j + 1;
      continue;
    }
    if (c == '<') {  // stray '<' in text ("x < y")
      out += "&lt;";
      i++;
      continue;
    }
    if (c == '&') {
      size_t entityEnd = 0;
      if (isEntityRef(html, i, &entityEnd)) {
        out.append(html, i, entityEnd - i + 1);
        i = entityEnd + 1;
      } else {  // bare ampersand ("Tom & Jerry")
        out += "&amp;";
        i++;
      }
      continue;
    }
    out += c;
    i++;
  }

  out += "</body></html>";
  return out;
}

}  // namespace

bool buildDictionaryHtmlPages(GfxRenderer& renderer, const std::string& definition, const uint16_t viewportWidth,
                              const uint16_t viewportHeight, std::vector<std::unique_ptr<Page>>& pagesOut) {
  const std::string xhtml = normalizeToXhtml(definition);

  {
    HalFile tmp = Storage.open(TMP_HTML_PATH, O_WRITE | O_CREAT | O_TRUNC);
    if (!tmp) {
      LOG_ERR("DHTML", "Cannot create %s", TMP_HTML_PATH);
      return false;
    }
    if (tmp.write(xhtml.data(), xhtml.size()) != static_cast<int>(xhtml.size())) {
      LOG_ERR("DHTML", "Short write to %s", TMP_HTML_PATH);
      return false;
    }
  }  // destructor closes the file before the parser reopens the same path

  pagesOut.clear();
  pagesOut.reserve(definition.size() / 800 + 4);  // ~a screenful of body text per page

  bool ok = false;
  {
    const std::string tmpPath = TMP_HTML_PATH;  // the parser stores a reference
    // Heap-allocated as Section does — the parser object is far too large for
    // a stack local. Null epub is safe: imageRendering=2 suppresses <img>
    // handling, the only path that dereferences it.
    auto parser = makeUniqueNoThrow<ChapterHtmlSlimParser>(
        nullptr, tmpPath, renderer, SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
        SETTINGS.extraParagraphSpacing, SETTINGS.paragraphAlignment, viewportWidth, viewportHeight,
        SETTINGS.hyphenationEnabled, SETTINGS.focusReadingEnabled,
        [&pagesOut](std::unique_ptr<Page> page, uint16_t, uint16_t, uint32_t) { pagesOut.push_back(std::move(page)); },
        /*embeddedStyle=*/false, /*contentBase=*/"", /*imageBasePath=*/"", /*imageRendering=*/2);
    if (!parser) {
      LOG_ERR("DHTML", "OOM: ChapterHtmlSlimParser");
    } else {
      ok = parser->parseAndBuildPages();  // closes the file on both outcomes
    }
  }
  Storage.remove(TMP_HTML_PATH);

  if (!ok || pagesOut.empty()) {
    pagesOut.clear();
    return false;
  }
  return true;
}
