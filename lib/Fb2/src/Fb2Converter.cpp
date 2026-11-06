#include "Fb2Converter.h"
#include "Fb2Passes.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <expat.h>

#include "Base64Decoder.h"
#include "Fb2Encoding.h"
#include "StoredZipWriter.h"

namespace {

using namespace fb2passes;  // parsing passes (Fb2Passes.h); emission stays here

constexpr size_t kReadChunk = 16384;
constexpr size_t kMaxNavTitleLen = 96;

bool parseFile(const std::string& path, XML_Parser p, std::string& error, const bool tolerateAbort = false) {
  HalFile f;
  if (!Storage.openFileForRead("FB2", path, f)) {
    error = "cannot open file";
    return false;
  }
  static char buf[kReadChunk];
  bool ok = true;
  size_t total = 0;
  for (;;) {
    const int n = f.read(buf, sizeof(buf));
    if (n < 0) {
      error = "read error";
      ok = false;
      break;
    }
    const bool isFinal = n == 0;
    if ((total & 0x3FFFF) == 0) LOG_DBG("FB2", "parsed %u KB", (unsigned)(total / 1024));
    if (XML_Parse(p, buf, (int)(n > 0 ? n : 0), isFinal) == XML_STATUS_ERROR) {
      if (tolerateAbort && XML_GetErrorCode(p) == XML_ERROR_ABORTED) break;  // early stop requested
      char lineInfo[48];
      snprintf(lineInfo, sizeof(lineInfo), " (line %lu)", (unsigned long)XML_GetCurrentLineNumber(p));
      error = std::string("XML: ") + XML_ErrorString(XML_GetErrorCode(p)) + lineInfo;
      ok = false;
      break;
    }
    total += (size_t)(n > 0 ? n : 0);
    if (isFinal) break;
  }
  f.close();
  return ok;
}

XML_Parser makeParser() {
  XML_Parser p = XML_ParserCreate(nullptr);
  if (p) XML_SetUnknownEncodingHandler(p, fb2UnknownEncodingHandler, nullptr);
  return p;
}

}  // namespace

const char* Fb2Converter::imageExtensionForContentType(const char* contentType) {
  // Kept for the public class API; the implementation lives in Fb2Passes.h.
  return fb2passes::imageExtensionForContentType(contentType);
}

bool Fb2Converter::readMetadata(const std::string& fb2Path, Metadata& meta, std::string& error) {
  XML_Parser p = makeParser();
  if (!p) {
    error = "parser alloc failed";
    return false;
  }
  MetaPass m;
  m.parser = p;
  XML_SetElementHandler(p, metaStart, metaEnd);
  XML_SetCharacterDataHandler(p, metaText);
  XML_SetUserData(p, &m);
  const bool ok = parseFile(fb2Path, p, error, true);
  XML_ParserFree(p);
  if (!ok) return false;
  meta = m.collect();
  return !meta.title.empty() || !meta.author.empty();
}

// ---------------------------------------------------------------- pass 3 ---
// Emission: walks the FB2 a third time, mirroring StructurePass chapter
// numbering exactly, and writes the synthetic EPUB.

namespace {

struct Frame {
  char close[16] = {};  // "" for transparent/self-contained elements
  bool carrier = false;
};

struct Materialized {
  int32_t logical = 0;
  char title[kMaxNavTitleLen + 1] = {};
};

constexpr size_t kMaxImageBytes = 16 * 1024 * 1024;

struct EmitPass {
  StoredZipWriter zip;
  const Fb2Converter::Metadata& meta;
  const StructurePass& structure;
  const char* bookId;  // stable urn:uuid:... tail

  int depth = 0;
  int bodyDepth = -1;
  int bodyCount = 0;
  int chapterCursor = -1;
  int sectionNesting = 0;
  bool inBody = false;
  bool sawRoot = false;

  std::vector<Frame> frames;
  int skipDepth = -1;
  int textCarrier = 0;

  bool inTitle = false;
  int titleLevel = 1;

  int chapterOpenIdx = -1;
  std::vector<Materialized> materialized;
  bool navCapture = false;
  int navCaptureFor = -2;  // chapterCursor whose nav label is being captured
  char navBuf[kMaxNavTitleLen + 1] = {};
  size_t navLen = 0;
  char pendingAnchor[64] = {};

  int binaryIdx = -1;
  size_t binaryBytes = 0;
  Base64Decoder b64;

  EmitPass(const Fb2Converter::Metadata& m, const StructurePass& s, const char* id)
      : meta(m), structure(s), bookId(id) {}

  // ---- chapter entry management ----
  bool ensureChapterOpen() {
    if (chapterOpenIdx == chapterCursor) return true;
    if (chapterOpenIdx >= 0) closeChapter();
    if (chapterCursor < 0) return false;
    char name[24];
    snprintf(name, sizeof(name), "s%d.xhtml", chapterCursor);
    LOG_DBG("FB2", "chapter open: %s", name);
    if (!zip.startEntry(name)) return false;
    if (!zip.writeStr("<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                      "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title></title></head>"
                      "<body>")) {
      return false;
    }
    if (pendingAnchor[0]) {
      if (!zip.writeFmt("<div id=\"%s\"></div>", pendingAnchor)) return false;
      pendingAnchor[0] = 0;
    }
    materialized.emplace_back();
    materialized.back().logical = chapterCursor;
    materialized.back().title[0] = 0;
    navCapture = false;
    navLen = 0;
    navBuf[0] = 0;
    chapterOpenIdx = chapterCursor;
    return true;
  }

  void closeChapter() {
    if (chapterOpenIdx < 0) return;
    zip.writeStr("</body></html>");
    zip.finishEntry();
    chapterOpenIdx = -1;
    if (!materialized.empty()) {
      memcpy(materialized.back().title, navBuf, sizeof(materialized.back().title));
    }
  }

  // ---- output primitives ----
  bool emitRaw(const char* s) {
    if (skipDepth >= 0) return true;
    if (!ensureChapterOpen()) return false;
    return zip.writeStr(s);
  }

  bool emitFmt(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
  void pushFrame(const char* close, bool carrier) {
    Frame f;
    snprintf(f.close, sizeof(f.close), "%s", close);
    f.carrier = carrier;
    frames.push_back(f);
    if (carrier) textCarrier++;
  }

  void writeNavTitleChunk(const char* s, int len) {
    if (!navCapture || navLen >= kMaxNavTitleLen) return;
    for (int i = 0; i < len && navLen < kMaxNavTitleLen; i++) {
      const char c = s[i];
      if (c == '\n' || c == '\r' || c == '\t') continue;
      if (c == ' ' && (navLen == 0 || navBuf[navLen - 1] == ' ')) continue;
      navBuf[navLen++] = c;
    }
    navBuf[navLen] = 0;
  }

  const char* mediaTypeForExt(const char* ext) const {
    if (strcmp(ext, ".jpg") == 0) return "image/jpeg";
    if (strcmp(ext, ".png") == 0) return "image/png";
    if (strcmp(ext, ".gif") == 0) return "image/gif";
    return "";
  }
};

bool EmitPass::emitFmt(const char* fmt, ...) {
  if (skipDepth >= 0) return true;
  if (!ensureChapterOpen()) return false;
  char buf[256];
  va_list args;
  va_start(args, fmt);
  const int n = vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (n <= 0) return false;
  const size_t len = (n >= (int)sizeof(buf)) ? sizeof(buf) - 1 : (size_t)n;
  return zip.write(buf, len);
}

void emitEscaped(EmitPass& e, const char* s, int len) {
  for (int i = 0; i < len; i++) {
    const char c = s[i];
    if (c == '&') e.emitRaw("&amp;");
    else if (c == '<') e.emitRaw("&lt;");
    else if (c == '>') e.emitRaw("&gt;");
    else {
      char one[2] = {c, 0};
      e.emitRaw(one);
    }
  }
}

void emitStartMarkup(EmitPass& e, const char* tag, const char* id, const char* style) {
  if (id && id[0]) {
    if (style && style[0]) e.emitFmt("<%s id=\"%s\" style=\"%s\">", tag, id, style);
    else e.emitFmt("<%s id=\"%s\">", tag, id);
  } else if (style && style[0]) {
    e.emitFmt("<%s style=\"%s\">", tag, style);
  } else {
    char buf[24];
    snprintf(buf, sizeof(buf), "<%s>", tag);
    e.emitRaw(buf);
  }
}

bool isExternalHref(const char* href) {
  return strncmp(href, "http://", 7) == 0 || strncmp(href, "https://", 8) == 0 ||
         strncmp(href, "mailto:", 7) == 0;
}

void XMLCALL emitStart(void* userData, const XML_Char* name, const char** attrs) {
  EmitPass& e = *static_cast<EmitPass*>(userData);
  const char* ln = localName(name);
  e.depth++;

  if (!e.sawRoot) {
    e.sawRoot = true;
    if (strcmp(ln, "FictionBook") != 0) return;  // tolerated; convert() checks the outcome
  }

  if (e.skipDepth >= 0) {
    e.pushFrame("", false);
    return;
  }

  const char* id = findAttr(attrs, "id");

  if (strcmp(ln, "description") == 0 || strcmp(ln, "stylesheet") == 0) {
    e.skipDepth = e.depth;
    e.pushFrame("", false);
    return;
  }
  if (strcmp(ln, "body") == 0) {
    e.bodyCount++;
    e.bodyDepth = e.depth;
    e.inBody = true;
    e.sectionNesting = 0;
    // Front-matter chapter slot for EVERY body (main title page, notes
    // header); lazy-opened, so bodies without front matter skip the entry.
    e.chapterCursor++;
    e.pushFrame("", false);
    return;
  }
  if (strcmp(ln, "section") == 0) {
    e.sectionNesting++;
    if (e.depth == e.bodyDepth + 1 && e.chapterCursor < kMaxChapters) {
      // New top-level section: close the previous chapter and start numbering
      // the next one. The entry opens lazily on the first real content, so
      // empty sections produce no blank chapters.
      e.closeChapter();
      e.chapterCursor++;
      if (id && id[0]) snprintf(e.pendingAnchor, sizeof(e.pendingAnchor), "%s", id);
    } else if (e.depth == e.bodyDepth + 1) {
      e.skipDepth = e.depth;  // chapter cap reached: drop the overflow section
    } else {
      emitStartMarkup(e, "div", id, nullptr);
      e.pushFrame("</div>", false);
      return;
    }
    e.pushFrame("", false);
    return;
  }
  if (strcmp(ln, "title") == 0) {
    e.inTitle = true;
    const int lvl = e.sectionNesting > 1 ? e.sectionNesting : 1;
    e.titleLevel = lvl > 6 ? 6 : lvl;
    if (e.chapterCursor >= 0 && e.navCaptureFor != e.chapterCursor) {
      e.navCapture = true;  // first title text becomes the nav label
      e.navCaptureFor = e.chapterCursor;
    }
    e.pushFrame("", false);
    return;
  }
  if (strcmp(ln, "p") == 0) {
    if (e.inTitle) {
      char tag[4], close[6];
      snprintf(tag, sizeof(tag), "h%d", e.titleLevel);
      snprintf(close, sizeof(close), "</%s>", tag);
      emitStartMarkup(e, tag, nullptr, nullptr);
      e.pushFrame(close, true);
    } else {
      emitStartMarkup(e, "p", id, nullptr);
      e.pushFrame("</p>", true);
    }
    return;
  }
  if (strcmp(ln, "subtitle") == 0) {
    emitStartMarkup(e, "h4", id, nullptr);
    e.pushFrame("</h4>", true);
    return;
  }
  if (strcmp(ln, "emphasis") == 0) {
    e.emitRaw("<em>");
    e.pushFrame("</em>", true);
    return;
  }
  if (strcmp(ln, "strong") == 0) {
    e.emitRaw("<strong>");
    e.pushFrame("</strong>", true);
    return;
  }
  if (strcmp(ln, "strikethrough") == 0) {
    e.emitRaw("<s>");
    e.pushFrame("</s>", true);
    return;
  }
  if (strcmp(ln, "sub") == 0) {
    e.emitRaw("<sub>");
    e.pushFrame("</sub>", true);
    return;
  }
  if (strcmp(ln, "sup") == 0) {
    e.emitRaw("<sup>");
    e.pushFrame("</sup>", true);
    return;
  }
  if (strcmp(ln, "style") == 0 || strcmp(ln, "code") == 0) {
    // code: no monospace face on-device; children render as regular text.
    e.pushFrame("", true);
    return;
  }
  if (strcmp(ln, "a") == 0) {
    const char* href = findAttr(attrs, "href");
    if (href && isExternalHref(href)) {
      e.emitFmt("<a href=\"%s\">", href);
      e.pushFrame("</a>", true);
    } else if (href && href[0] == '#') {
      const int32_t ch = e.structure.findIdChapter(href + 1);
      if (ch < 0) {
        e.pushFrame("", true);  // unknown target: plain text
      } else if (ch == e.chapterCursor) {
        e.emitFmt("<a href=\"#%s\">", href + 1);
        e.pushFrame("</a>", true);
      } else {
        e.emitFmt("<a href=\"s%d.xhtml#%s\">", ch, href + 1);
        e.pushFrame("</a>", true);
      }
    } else {
      e.pushFrame("", true);
    }
    return;
  }
  if (strcmp(ln, "image") == 0) {
    const char* href = findAttr(attrs, "href");
    if (href && href[0] == '#') {
      const int idx = e.structure.findBinary(href + 1);
      if (idx >= 0 && e.structure.binaries[idx].ext[0]) {
        e.emitFmt("<img src=\"img/%d%s\" alt=\"\"/>", idx, e.structure.binaries[idx].ext);
      }
    }
    e.pushFrame("", false);
    return;
  }
  if (strcmp(ln, "poem") == 0) {
    emitStartMarkup(e, "div", id, "margin:0.4em 0");
    e.pushFrame("</div>", false);
    return;
  }
  if (strcmp(ln, "stanza") == 0) {
    emitStartMarkup(e, "div", id, "margin:0.25em 0");
    e.pushFrame("</div>", false);
    return;
  }
  if (strcmp(ln, "v") == 0) {
    emitStartMarkup(e, "p", id, "text-indent:0;margin:0");
    e.pushFrame("</p>", true);
    return;
  }
  if (strcmp(ln, "empty-line") == 0) {
    e.emitRaw("<p style=\"text-indent:0;margin:0\"><br/></p>");
    e.pushFrame("", false);
    return;
  }
  if (strcmp(ln, "text-author") == 0) {
    emitStartMarkup(e, "p", id, "text-align:right;text-indent:0;margin:0");
    e.emitRaw("<em>");
    e.pushFrame("</em></p>", true);
    return;
  }
  if (strcmp(ln, "epigraph") == 0) {
    emitStartMarkup(e, "blockquote", id, "margin:0.5em 1.5em");
    e.pushFrame("</blockquote>", false);
    return;
  }
  if (strcmp(ln, "cite") == 0) {
    emitStartMarkup(e, "blockquote", id, nullptr);
    e.pushFrame("</blockquote>", false);
    return;
  }
  if (strcmp(ln, "table") == 0) {
    emitStartMarkup(e, "table", id, nullptr);
    e.pushFrame("</table>", false);
    return;
  }
  if (strcmp(ln, "tr") == 0) {
    e.emitRaw("<tr>");
    e.pushFrame("</tr>", false);
    return;
  }
  if (strcmp(ln, "td") == 0 || strcmp(ln, "th") == 0) {
    const char* align = findAttr(attrs, "align");
    const char* tag = ln[0] == 't' && ln[1] == 'd' ? "td" : "th";
    const char* close = ln[0] == 't' && ln[1] == 'd' ? "</td>" : "</th>";
    if (align && (strcmp(align, "center") == 0 || strcmp(align, "right") == 0 || strcmp(align, "left") == 0)) {
      e.emitFmt("<%s style=\"text-align:%s\">", tag, align);
    } else {
      e.emitFmt("<%s>", tag);
    }
    e.pushFrame(close, true);
    return;
  }
  if (strcmp(ln, "binary") == 0) {
    // By schema binaries follow the bodies, so no chapter entry is open. A
    // binary encountered mid-body is pathological: skip it whole.
    const char* binId = findAttr(attrs, "id");
    const char* ct = findAttr(attrs, "content-type");
    const int idx = binId ? e.structure.findBinary(binId) : -1;
    const bool okBinary = idx >= 0 && e.structure.binaries[idx].ext[0] && ct && e.chapterOpenIdx < 0;
    if (okBinary) {
      char entry[24];
      snprintf(entry, sizeof(entry), "img/%d%s", idx, e.structure.binaries[idx].ext);
      if (e.zip.startEntry(entry)) {
        e.binaryIdx = idx;
        e.binaryBytes = 0;
        e.b64 = Base64Decoder();
        e.pushFrame("", false);
        return;
      }
    }
    e.skipDepth = e.depth;
    e.pushFrame("", false);
    return;
  }

  // Unknown element: transparent container, children still processed.
  e.pushFrame("", false);
}

void XMLCALL emitEnd(void* userData, const XML_Char* name) {
  EmitPass& e = *static_cast<EmitPass*>(userData);
  const char* ln = localName(name);

  if (e.binaryIdx >= 0 && strcmp(ln, "binary") == 0) {
    e.b64.finish([&e](const uint8_t* data, size_t len) {
      e.binaryBytes += len;
      if (e.binaryBytes > kMaxImageBytes) return false;
      return e.zip.write(data, len);
    });
    e.zip.finishEntry();
    e.binaryIdx = -1;
  } else if (strcmp(ln, "title") == 0) {
    e.inTitle = false;
    e.navCapture = false;
  } else if (strcmp(ln, "body") == 0) {
    e.inBody = false;
    e.bodyDepth = -1;
    e.closeChapter();  // body end: nothing may append to its last chapter
  } else if (strcmp(ln, "section") == 0) {
    if (e.sectionNesting > 0) e.sectionNesting--;
    // Top-level sections do NOT close the chapter here: the next section's
    // start (or the body end) does. Keeping the entry open lets stray
    // body-level content between sections append to the current chapter
    // instead of re-opening the same entry name twice in the ZIP.
  }

  if (!e.frames.empty()) {
    const Frame f = e.frames.back();
    e.frames.pop_back();
    if (f.carrier && e.textCarrier > 0) e.textCarrier--;
    if (f.close[0] && e.skipDepth < 0 && e.binaryIdx < 0) {
      e.emitRaw(f.close);
    }
  }

  if (e.skipDepth == e.depth) e.skipDepth = -1;
  e.depth--;
}

void XMLCALL emitText(void* userData, const XML_Char* s, int len) {
  EmitPass& e = *static_cast<EmitPass*>(userData);
  if (len <= 0) return;
  if (e.binaryIdx >= 0) {
    e.b64.feed(s, len, [&e](const uint8_t* data, size_t n) {
      e.binaryBytes += n;
      if (e.binaryBytes > kMaxImageBytes) return false;
      return e.zip.write(data, n);
    });
    return;
  }
  if (e.skipDepth >= 0 || e.textCarrier <= 0) return;  // drop inter-block whitespace
  emitEscaped(e, s, len);
  if (e.navCapture) e.writeNavTitleChunk(s, len);
}

}  // namespace

// ------------------------------------------------------------ convert() ---

namespace {

// XML-escape `s` into the entry, flushing through a small buffer.
bool writeXmlEscaped(StoredZipWriter& zip, const char* s) {
  char buf[64];
  size_t n = 0;
  for (const char* p = s; *p; p++) {
    const char* rep = nullptr;
    if (*p == '&') rep = "&amp;";
    else if (*p == '<') rep = "&lt;";
    else if (*p == '>') rep = "&gt;";
    else if (*p == '"') rep = "&quot;";
    if (rep) {
      const size_t rl = strlen(rep);
      if (n + rl > sizeof(buf)) {
        if (!zip.write(buf, n)) return false;
        n = 0;
      }
      memcpy(buf + n, rep, rl);
      n += rl;
    } else {
      if (n + 1 > sizeof(buf)) {
        if (!zip.write(buf, n)) return false;
        n = 0;
      }
      buf[n++] = *p;
    }
  }
  return n == 0 || zip.write(buf, n);
}

}  // namespace

bool Fb2Converter::convert(const std::string& fb2Path, const std::string& outPath, std::string& error) {
  error.clear();
  const uint32_t t0 = millis();

  // Pass 1: metadata (a missing title-info is tolerated; XML errors are not).
  Metadata meta;
  const bool haveMeta = readMetadata(fb2Path, meta, error);
  if (!error.empty()) return false;
  LOG_DBG("FB2", "pass1 metadata (%lu ms): %s, lang=%s, cover=%s, series=%s", millis() - t0,
          meta.title.c_str(), meta.language.c_str(), meta.coverRef.c_str(), meta.seriesName.c_str());
  if (!haveMeta || meta.title.empty()) {
    const char* base = fb2Path.c_str();
    const char* slash = strrchr(base, '/');
    if (slash) base = slash + 1;
    meta.title = base;
    const size_t dot = meta.title.rfind('.');
    if (dot != std::string::npos && meta.title.size() - dot <= 4) meta.title.resize(dot);
    if (meta.title.empty()) meta.title = "FB2 book";
  }
  if (meta.language.empty()) meta.language = "ru";

  // Pass 2: structure.
  StructurePass st;
  {
    XML_Parser p = makeParser();
    if (!p) {
      error = "parser alloc failed";
      return false;
    }
    XML_SetElementHandler(p, structStart, structEnd);
    XML_SetUserData(p, &st);
    const bool ok = parseFile(fb2Path, p, error);
    XML_ParserFree(p);
    if (!ok) return false;
  }
  if (strcmp(st.rootName, "FictionBook") != 0) {
    error = "not a FictionBook document";
    return false;
  }
  LOG_DBG("FB2", "pass2 structure (%lu ms): chapters=%d binaries=%u ids=%u", millis() - t0,
          st.chapterCursor + 1, (unsigned)st.binaries.size(), (unsigned)st.idTargets.size());

  // Pass 3: emission.
  char bookId[24];
  snprintf(bookId, sizeof(bookId), "%08x", (unsigned)std::hash<std::string>{}(fb2Path));
  EmitPass e(meta, st, bookId);
  if (!e.zip.open(outPath)) {
    error = "cannot create cache file";
    return false;
  }
  bool parsed = false;
  {
    XML_Parser p = makeParser();
    if (!p) {
      error = "parser alloc failed";
      e.zip.abort();
      return false;
    }
    XML_SetElementHandler(p, emitStart, emitEnd);
    XML_SetCharacterDataHandler(p, emitText);
    XML_SetUserData(p, &e);
    LOG_DBG("FB2", "pass3 emission start");
    parsed = parseFile(fb2Path, p, error);
    XML_ParserFree(p);
  }
  e.closeChapter();
  LOG_DBG("FB2", "pass3 done (%lu ms): parsed=%d materialized=%u", millis() - t0, (int)parsed,
          (unsigned)e.materialized.size());
  if (!parsed || !e.zip.ok()) {
    e.zip.abort();
    Storage.remove(outPath.c_str());
    if (parsed) error = "write failed during conversion";
    return false;
  }
  if (e.materialized.empty()) {
    e.zip.abort();
    Storage.remove(outPath.c_str());
    error = "no readable body content";
    return false;
  }

  // nav.xhtml
  if (!e.zip.startEntry("nav.xhtml") ||
      !e.zip.writeStr("<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                      "<html xmlns=\"http://www.w3.org/1999/xhtml\" "
                      "xmlns:epub=\"http://www.idpf.org/2007/ops\">"
                      "<head><title>nav</title></head><body>"
                      "<nav epub:type=\"toc\" id=\"nav\"><ol>")) {
    e.zip.abort();
    error = "nav write failed";
    return false;
  }
  for (const Materialized& m : e.materialized) {
    if (!e.zip.writeFmt("<li><a href=\"s%d.xhtml\">", m.logical)) break;
    if (m.title[0]) {
      if (!writeXmlEscaped(e.zip, m.title)) break;
    } else {
      if (!e.zip.writeFmt("%d", m.logical)) break;
    }
    if (!e.zip.writeStr("</a></li>\n")) break;
  }
  if (!e.zip.writeStr("</ol></nav></body></html>") || !e.zip.finishEntry()) {
    e.zip.abort();
    error = "nav write failed";
    return false;
  }

  // META-INF/container.xml
  if (!e.zip.startEntry("META-INF/container.xml") ||
      !e.zip.writeStr("<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                      "<container version=\"1.0\" "
                      "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">"
                      "<rootfiles><rootfile full-path=\"content.opf\" "
                      "media-type=\"application/oebps-package+xml\"/></rootfiles></container>") ||
      !e.zip.finishEntry()) {
    e.zip.abort();
    error = "container write failed";
    return false;
  }

  // mimetype (EPUB validity; readers locate parts through the central dir)
  if (!e.zip.startEntry("mimetype") || !e.zip.writeStr("application/epub+zip") || !e.zip.finishEntry()) {
    e.zip.abort();
    error = "mimetype write failed";
    return false;
  }

  const int coverIdx = meta.coverRef.empty() ? -1 : st.findBinary(meta.coverRef.c_str());
  const bool haveCover = coverIdx >= 0 && st.binaries[coverIdx].ext[0];

  // content.opf
  if (!e.zip.startEntry("content.opf") ||
      !e.zip.writeStr("<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                      "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" "
                      "unique-identifier=\"uid\">"
                      "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
                      "<dc:identifier id=\"uid\">urn:uuid:fb2-") ||
      !e.zip.writeStr(bookId) || !e.zip.writeStr("</dc:identifier><dc:title>") ||
      !writeXmlEscaped(e.zip, meta.title.c_str()) || !e.zip.writeStr("</dc:title>")) {
    e.zip.abort();
    error = "opf write failed";
    return false;
  }
  if (!meta.author.empty()) {
    if (!e.zip.writeStr("<dc:creator>") || !writeXmlEscaped(e.zip, meta.author.c_str()) ||
        !e.zip.writeStr("</dc:creator>")) {
      e.zip.abort();
      error = "opf write failed";
      return false;
    }
  }
  if (!e.zip.writeFmt("<dc:language>%s</dc:language>", meta.language.c_str())) {
    e.zip.abort();
    error = "opf write failed";
    return false;
  }
  if (!meta.seriesName.empty()) {
    const char* pos = meta.seriesNumber.empty() ? "1" : meta.seriesNumber.c_str();
    if (!e.zip.writeStr("<meta property=\"belongs-to-collection\" id=\"ser\">") ||
        !writeXmlEscaped(e.zip, meta.seriesName.c_str()) ||
        !e.zip.writeStr("</meta><meta refines=\"#ser\" property=\"collection-type\">series</meta>"
                        "<meta refines=\"#ser\" property=\"group-position\">") ||
        !writeXmlEscaped(e.zip, pos) ||
        !e.zip.writeStr("</meta><meta name=\"calibre:series\" content=\"") ||
        !writeXmlEscaped(e.zip, meta.seriesName.c_str()) ||
        !e.zip.writeStr("\"/><meta name=\"calibre:series_index\" content=\"") ||
        !writeXmlEscaped(e.zip, pos) ||
        !e.zip.writeStr("\"/>")) {
      e.zip.abort();
      error = "opf write failed";
      return false;
    }
  }
  if (haveCover && !e.zip.writeStr("<meta name=\"cover\" content=\"cover-img\"/>")) {
    e.zip.abort();
    error = "opf write failed";
    return false;
  }

  // manifest
  if (!e.zip.writeStr("</metadata><manifest>"
                      "<item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" "
                      "properties=\"nav\"/>")) {
    e.zip.abort();
    error = "opf write failed";
    return false;
  }
  for (const Materialized& m : e.materialized) {
    if (!e.zip.writeFmt("<item id=\"c%d\" href=\"s%d.xhtml\" media-type=\"application/xhtml+xml\"/>",
                        m.logical, m.logical)) {
      e.zip.abort();
      error = "opf write failed";
      return false;
    }
  }
  for (size_t i = 0; i < st.binaries.size(); i++) {
    const BinaryInfo& b = st.binaries[i];
    if (!b.ext[0]) continue;
    const char* mt = nullptr;
    {
      if (strcmp(b.ext, ".jpg") == 0) mt = "image/jpeg";
      else if (strcmp(b.ext, ".png") == 0) mt = "image/png";
      else mt = "image/gif";
    }
    if ((int)i == coverIdx) {
      if (!e.zip.writeFmt("<item id=\"cover-img\" href=\"img/%d%s\" media-type=\"%s\" "
                          "properties=\"cover-image\"/>",
                          (int)i, b.ext, mt)) {
        e.zip.abort();
        error = "opf write failed";
        return false;
      }
    } else if (!e.zip.writeFmt("<item id=\"im%d\" href=\"img/%d%s\" media-type=\"%s\"/>", (int)i, (int)i,
                               b.ext, mt)) {
      e.zip.abort();
      error = "opf write failed";
      return false;
    }
  }

  // spine
  if (!e.zip.writeStr("</manifest><spine>")) {
    e.zip.abort();
    error = "opf write failed";
    return false;
  }
  for (const Materialized& m : e.materialized) {
    if (!e.zip.writeFmt("<itemref idref=\"c%d\"/>", m.logical)) {
      e.zip.abort();
      error = "opf write failed";
      return false;
    }
  }
  if (!e.zip.writeStr("</spine></package>") || !e.zip.finishEntry()) {
    e.zip.abort();
    error = "opf write failed";
    return false;
  }

  if (!e.zip.finish()) {
    e.zip.abort();
    Storage.remove(outPath.c_str());
    error = "zip finalize failed";
    return false;
  }

  LOG_INF("FB2", "converted in %lu ms: %u chapters, %u images, %u anchors", millis() - t0,
          (unsigned)e.materialized.size(), (unsigned)st.binaries.size(),
          (unsigned)st.idTargets.size());
  return true;
}