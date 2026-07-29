#include "MobiToEpub.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <ZipWriter.h>

#include <cctype>
#include <cstring>
#include <string>
#include <vector>

#include "MobiParser.h"

namespace {

// Bumped whenever the converter's output changes: the generated EPUB keeps a
// stable path, so without this a stale conversion (and the reader's section
// caches derived from it) would survive a firmware upgrade.
constexpr uint32_t CONVERTER_VERSION = 2;

constexpr size_t CHAPTER_SOFT_LIMIT = 160 * 1024;  // split oversized flows at block boundaries
// Hard ceiling: a book whose whole body sits inside one never-closed <div> has
// no block boundary to split at, so the soft limit alone would grow the buffer
// to the entire book and abort() on OOM. At the ceiling we split mid-flow and
// re-open the tag stack in the next chapter.
constexpr size_t CHAPTER_HARD_LIMIT = 320 * 1024;
constexpr size_t MAX_TAG_NAME = 64;
constexpr size_t MAX_ATTR_NAME = 64;
constexpr size_t MAX_ATTR_VALUE = 512;

// ---------------------------------------------------------------------------
// Entities: named -> Unicode codepoint. Unknown names are emitted as literal
// "&amp;name;" so no text is ever lost and expat never sees a bad entity.
struct NamedEntity {
  const char* name;
  uint16_t cp;
};
constexpr NamedEntity ENTITIES[] = {
    {"amp", '&'},      {"lt", '<'},       {"gt", '>'},       {"quot", '"'},     {"apos", '\''},
    {"nbsp", 160},     {"iexcl", 161},    {"cent", 162},     {"pound", 163},    {"curren", 164},
    {"yen", 165},      {"brvbar", 166},   {"sect", 167},     {"uml", 168},      {"copy", 169},
    {"ordf", 170},     {"laquo", 171},    {"not", 172},      {"shy", 173},      {"reg", 174},
    {"macr", 175},     {"deg", 176},      {"plusmn", 177},   {"sup2", 178},     {"sup3", 179},
    {"acute", 180},    {"micro", 181},    {"para", 182},     {"middot", 183},   {"cedil", 184},
    {"sup1", 185},     {"ordm", 186},     {"raquo", 187},    {"frac14", 188},   {"frac12", 189},
    {"frac34", 190},   {"iquest", 191},   {"Agrave", 192},   {"Aacute", 193},   {"Acirc", 194},
    {"Atilde", 195},   {"Auml", 196},     {"Aring", 197},    {"AElig", 198},    {"Ccedil", 199},
    {"Egrave", 200},   {"Eacute", 201},   {"Ecirc", 202},    {"Euml", 203},     {"Igrave", 204},
    {"Iacute", 205},   {"Icirc", 206},    {"Iuml", 207},     {"ETH", 208},      {"Ntilde", 209},
    {"Ograve", 210},   {"Oacute", 211},   {"Ocirc", 212},    {"Otilde", 213},   {"Ouml", 214},
    {"times", 215},    {"Oslash", 216},   {"Ugrave", 217},   {"Uacute", 218},   {"Ucirc", 219},
    {"Uuml", 220},     {"Yacute", 221},   {"THORN", 222},    {"szlig", 223},    {"agrave", 224},
    {"aacute", 225},   {"acirc", 226},    {"atilde", 227},   {"auml", 228},     {"aring", 229},
    {"aelig", 230},    {"ccedil", 231},   {"egrave", 232},   {"eacute", 233},   {"ecirc", 234},
    {"euml", 235},     {"igrave", 236},   {"iacute", 237},   {"icirc", 238},    {"iuml", 239},
    {"eth", 240},      {"ntilde", 241},   {"ograve", 242},   {"oacute", 243},   {"ocirc", 244},
    {"otilde", 245},   {"ouml", 246},     {"divide", 247},   {"oslash", 248},   {"ugrave", 249},
    {"uacute", 250},   {"ucirc", 251},    {"uuml", 252},     {"yacute", 253},   {"thorn", 254},
    {"yuml", 255},     {"OElig", 338},    {"oelig", 339},    {"Scaron", 352},   {"scaron", 353},
    {"Yuml", 376},     {"fnof", 402},     {"circ", 710},     {"tilde", 732},    {"ndash", 8211},
    {"mdash", 8212},   {"lsquo", 8216},   {"rsquo", 8217},   {"sbquo", 8218},   {"ldquo", 8220},
    {"rdquo", 8221},   {"bdquo", 8222},   {"dagger", 8224},  {"Dagger", 8225},  {"bull", 8226},
    {"hellip", 8230},  {"permil", 8240},  {"prime", 8242},   {"Prime", 8243},   {"lsaquo", 8249},
    {"rsaquo", 8250},  {"oline", 8254},   {"frasl", 8260},   {"euro", 8364},    {"trade", 8482},
    {"minus", 8722},   {"infin", 8734},   {"ne", 8800},      {"le", 8804},      {"ge", 8805},
};

int lookupEntity(const char* name, size_t len) {
  for (const auto& e : ENTITIES) {
    if (strlen(e.name) == len && memcmp(e.name, name, len) == 0) return e.cp;
  }
  return -1;
}

// cp1252 0x80-0x9F -> Unicode (0 = undefined -> U+FFFD later).
constexpr uint16_t CP1252_HIGH[32] = {8364, 0,    8218, 402,  8222, 8230, 8224, 8225, 710,  8240, 352,
                                      8249, 338,  0,    381,  0,    0,    8216, 8217, 8220, 8221, 8226,
                                      8211, 8212, 732,  8482, 353,  8250, 339,  0,    382,  376};

void appendUtf8(std::string& out, uint32_t cp) {
  if (cp == 0) cp = 0xFFFD;
  if (cp < 0x80) {
    out += (char)cp;
  } else if (cp < 0x800) {
    out += (char)(0xC0 | (cp >> 6));
    out += (char)(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += (char)(0xE0 | (cp >> 12));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
  } else {
    out += (char)(0xF0 | (cp >> 18));
    out += (char)(0x80 | ((cp >> 12) & 0x3F));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
  }
}

// XML-escape a codepoint into the chapter buffer.
void appendTextChar(std::string& out, uint32_t cp) {
  switch (cp) {
    case '&': out += "&amp;"; break;
    case '<': out += "&lt;"; break;
    case '>': out += "&gt;"; break;
    default: appendUtf8(out, cp);
  }
}

// ---------------------------------------------------------------------------
// Tag policy

enum class TagKind : uint8_t { Strip, Inline, Block, Void, Image, Anchor, PageBreak };

struct TagRule {
  const char* name;
  TagKind kind;
  const char* rename;  // nullptr = keep name
};

// Whitelist. Anything not listed is stripped (tag dropped, content kept).
constexpr TagRule TAG_RULES[] = {
    {"p", TagKind::Block, nullptr},        {"h1", TagKind::Block, nullptr},
    {"h2", TagKind::Block, nullptr},       {"h3", TagKind::Block, nullptr},
    {"h4", TagKind::Block, nullptr},       {"h5", TagKind::Block, nullptr},
    {"h6", TagKind::Block, nullptr},       {"div", TagKind::Block, nullptr},
    {"blockquote", TagKind::Block, nullptr}, {"center", TagKind::Block, "div"},
    {"ul", TagKind::Block, nullptr},       {"ol", TagKind::Block, nullptr},
    {"li", TagKind::Block, nullptr},       {"pre", TagKind::Block, "p"},
    {"b", TagKind::Inline, nullptr},       {"strong", TagKind::Inline, nullptr},
    {"i", TagKind::Inline, nullptr},       {"em", TagKind::Inline, nullptr},
    {"u", TagKind::Inline, nullptr},       {"s", TagKind::Inline, nullptr},
    {"strike", TagKind::Inline, "s"},      {"small", TagKind::Inline, nullptr},
    {"big", TagKind::Inline, "span"},      {"span", TagKind::Inline, nullptr},
    {"sup", TagKind::Inline, nullptr},     {"sub", TagKind::Inline, nullptr},
    {"font", TagKind::Inline, "span"},     {"code", TagKind::Inline, nullptr},
    {"cite", TagKind::Inline, nullptr},    {"tt", TagKind::Inline, "code"},
    {"br", TagKind::Void, nullptr},        {"hr", TagKind::Void, nullptr},
    {"img", TagKind::Image, nullptr},      {"a", TagKind::Anchor, nullptr},
    {"mbp:pagebreak", TagKind::PageBreak, nullptr},
};

const TagRule* findRule(const std::string& name) {
  for (const auto& r : TAG_RULES) {
    if (name == r.name) return &r;
  }
  return nullptr;
}

bool isBlockKind(TagKind k) { return k == TagKind::Block; }

struct Attr {
  std::string name;
  std::string value;
};

// ---------------------------------------------------------------------------

struct EpubBuilder {
  ZipWriter zip;
  MobiParser& mobi;
  std::string chapterBuf;
  std::vector<std::string> chapterTitles;
  std::vector<uint16_t> usedImages;  // recindex values, order of first use
  std::vector<std::string> openTags; // emitted-tag stack for auto-close
  std::string pendingHeadingText;    // first heading text of current chapter
  bool inHeading = false;
  int chapterCount = 0;
  bool failed = false;

  explicit EpubBuilder(MobiParser& m) : mobi(m) {}

  const char* imageExt(uint16_t recindex) {
    // Cheap signature re-check when writing; during tidy assume jpg and fix
    // the manifest at write time instead — keep a single source of truth by
    // probing here (records are small to probe? no — probe = full read).
    // Simpler: extension chosen at write time; chapters reference by index
    // with .img extension-neutral name is invalid for some readers, so we
    // standardize on the real extension recorded at write time. During tidy
    // we don't know it yet — so images are collected first and chapters
    // reference "images/imgNNNNN" + ext placeholder resolved in a second
    // pass. To avoid the two-pass complexity, use ".jpg" name for all —
    // renderers key on content, but CrossPoint keys on extension.
    (void)recindex;
    return nullptr;  // unused; see imageName()
  }

  // recindex -> zip-local name. Extension resolved lazily from record bytes
  // (cached), so chapter references and manifest entries agree.
  struct ImgInfo {
    uint16_t recindex;
    std::string name;
    std::string media;
  };
  std::vector<ImgInfo> images;

  const ImgInfo* ensureImage(uint16_t recindex) {
    for (const auto& im : images) {
      if (im.recindex == recindex) return &im;
    }
    auto bytes = mobi.readImage(recindex);
    if (bytes.empty()) return nullptr;
    const char* ext = "jpg";
    const char* media = "image/jpeg";
    if (bytes[0] == 0x89) {
      ext = "png";
      media = "image/png";
    } else if (bytes[0] == 'G') {
      ext = "gif";
      media = "image/gif";
      LOG_ERR("MOBI", "image %u is GIF - reader decodes only JPEG/PNG, it will not render", recindex);
    } else if (bytes[0] == 'B') {
      ext = "bmp";
      media = "image/bmp";
      LOG_ERR("MOBI", "image %u is BMP - reader decodes only JPEG/PNG, it will not render", recindex);
    }
    char name[32];
    snprintf(name, sizeof(name), "images/img%05u.%s", recindex, ext);
    if (!zip.addFile((std::string("OEBPS/") + name).c_str(), bytes.data(), bytes.size())) return nullptr;
    images.push_back({recindex, name, media});
    return &images.back();
  }

  void openChapter() {
    chapterBuf.clear();
    chapterBuf +=
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<html xmlns=\"http://www.w3.org/1999/xhtml\">\n<head><title>c</title></head>\n<body>\n";
    pendingHeadingText.clear();
    inHeading = false;
    openTags.clear();
  }

  void closeAllTags() {
    while (!openTags.empty()) {
      chapterBuf += "</" + openTags.back() + ">";
      openTags.pop_back();
    }
  }

  bool flushChapter() {
    if (chapterCount >= 9999) {
      // Never silently drop the tail and then cache it as a good conversion.
      failed = true;
      return false;
    }
    closeAllTags();
    chapterBuf += "\n</body>\n</html>\n";
    char name[40];
    snprintf(name, sizeof(name), "OEBPS/chapter%04d.xhtml", chapterCount);
    if (!zip.addFile(name, (const uint8_t*)chapterBuf.data(), chapterBuf.size())) {
      failed = true;
      return false;
    }
    chapterTitles.push_back(pendingHeadingText);
    chapterCount++;
    openChapter();
    vTaskDelay(1);  // yield: keep the watchdog + render task happy on big books
    return true;
  }

  // Chapter has real content beyond the skeleton?
  bool chapterHasContent() const { return chapterBuf.size() > 120; }

  // Split mid-flow when no block boundary has appeared: close the open tags,
  // flush, then re-open the same tags in the fresh chapter so formatting and
  // nesting survive the cut.
  void forceSplit() {
    std::vector<std::string> saved = openTags;
    flushChapter();
    for (const auto& t : saved) {
      chapterBuf += "<" + t + ">";
      openTags.push_back(t);
    }
  }
};

// Parses one tag starting at html[i] == '<'. Returns index past '>'.
size_t parseTag(const char* html, size_t len, size_t i, std::string& name, std::vector<Attr>& attrs, bool& closing,
                bool& selfClose) {
  name.clear();
  attrs.clear();
  closing = false;
  selfClose = false;
  i++;  // past '<'
  if (i < len && html[i] == '/') {
    closing = true;
    i++;
  }
  while (i < len && (isalnum((unsigned char)html[i]) || html[i] == ':' || html[i] == '-')) {
    if (name.size() < MAX_TAG_NAME) name += (char)tolower((unsigned char)html[i]);
    i++;
  }
  // Attributes
  while (i < len && html[i] != '>') {
    if (html[i] == '/') {
      // '/' inside a tag: '/>' closes it, a stray '/' (e.g. "<hr / >", a URL in
      // a comment) must still advance or the loop spins forever.
      if (i + 1 < len && html[i + 1] == '>') selfClose = true;
      i++;
      continue;
    }
    if (isspace((unsigned char)html[i])) {
      i++;
      continue;
    }
    Attr a;
    while (i < len && html[i] != '=' && html[i] != '>' && html[i] != '/' && !isspace((unsigned char)html[i])) {
      if (a.name.size() < MAX_ATTR_NAME) a.name += (char)tolower((unsigned char)html[i]);
      i++;
    }
    if (a.name.empty() && i < len && html[i] != '=' && html[i] != '>') {
      i++;  // junk char — must advance or we spin forever
      continue;
    }
    while (i < len && isspace((unsigned char)html[i])) i++;
    if (i < len && html[i] == '=') {
      i++;
      while (i < len && isspace((unsigned char)html[i])) i++;
      if (i < len && (html[i] == '"' || html[i] == '\'')) {
        const char q = html[i++];
        // An unterminated quote would otherwise copy the rest of the book into
        // this string; only the first MAX_ATTR_VALUE bytes are ever used.
        while (i < len && html[i] != q) {
          if (a.value.size() < MAX_ATTR_VALUE) a.value += html[i];
          i++;
        }
        if (i < len) i++;
      } else {
        while (i < len && html[i] != '>' && !isspace((unsigned char)html[i])) {
          if (a.value.size() < MAX_ATTR_VALUE) a.value += html[i];
          i++;
        }
      }
    }
    if (!a.name.empty()) attrs.push_back(std::move(a));
  }
  return i < len ? i + 1 : len;
}

void emitAttrValue(std::string& out, const std::string& v) {
  for (char c : v) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '"': out += "&quot;"; break;
      default: out += c;
    }
  }
}

}  // namespace

std::string MobiToEpub::ensureConverted(const std::string& mobiPath, std::string* errorOut) {
  auto setError = [&](const std::string& e) {
    if (errorOut) *errorOut = e;
    LOG_ERR("MOBI", "%s", e.c_str());
  };

  MobiParser mobi(mobiPath);
  if (!mobi.load()) {
    setError(mobi.errorString());
    return "";
  }

  // Cache: keyed on path hash; validated on source size + text length.
  const uint32_t hash = (uint32_t)std::hash<std::string>{}(mobiPath);
  char cacheDir[48];
  snprintf(cacheDir, sizeof(cacheDir), "/.crosspoint/mobi_%08x", (unsigned)hash);
  const std::string epubPath = std::string(cacheDir) + "/book.epub";
  const std::string tagPath = std::string(cacheDir) + "/src.bin";

  uint32_t srcTag[2] = {0, 0};
  {
    HalFile f;
    if (Storage.openFileForRead("MOBI", tagPath.c_str(), f)) {
      if (f.read(srcTag, sizeof(srcTag)) == (int)sizeof(srcTag) && srcTag[0] == CONVERTER_VERSION &&
          srcTag[1] == mobi.rawTextLength() && Storage.exists(epubPath.c_str())) {
        return epubPath;  // valid cached conversion
      }
    }
  }

  LOG_INF("MOBI", "converting %s (text %lu bytes)", mobiPath.c_str(), (unsigned long)mobi.rawTextLength());
  Storage.ensureDirectoryExists(cacheDir);

  // 1) Decompress the full HTML stream (PSRAM — big allocs route there).
  const uint32_t rawLen = mobi.rawTextLength();
  if (rawLen == 0 || rawLen > 24u * 1024 * 1024) {
    setError("book text missing or implausibly large");
    return "";
  }
  auto raw = makeUniqueNoThrow<uint8_t[]>(rawLen);
  if (!raw) {
    setError("out of memory for book text");
    return "";
  }
  const uint32_t got = mobi.readRawText(raw.get(), rawLen);
  if (got == 0) {
    setError(mobi.errorString());
    return "";
  }

  // 2) Tidy + split + write EPUB.
  EpubBuilder b(mobi);
  if (!b.zip.begin(epubPath.c_str())) {
    setError("cannot write EPUB");
    return "";
  }
  // EPUB structural entries. mimetype MUST be first + stored (it is: all
  // entries are stored).
  static const char MIMETYPE[] = "application/epub+zip";
  static const char CONTAINER[] =
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
      "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
      "<rootfiles><rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/>"
      "</rootfiles>\n</container>\n";
  if (!b.zip.addFile("mimetype", (const uint8_t*)MIMETYPE, sizeof(MIMETYPE) - 1) ||
      !b.zip.addFile("META-INF/container.xml", (const uint8_t*)CONTAINER, sizeof(CONTAINER) - 1)) {
    setError("cannot write EPUB");
    return "";
  }

  const bool cp1252 = true;  // MOBI textEncoding 1252 vs 65001 handled below
  const bool isUtf8 = false;
  (void)cp1252;
  (void)isUtf8;

  b.openChapter();
  const char* html = (const char*)raw.get();
  const size_t len = got;
  std::string tagName;
  std::vector<Attr> attrs;

  size_t i = 0;
  while (i < len && !b.failed) {
    const char c = html[i];
    if (c == '<') {
      bool closing = false, selfClose = false;
      const size_t next = parseTag(html, len, i, tagName, attrs, closing, selfClose);
      // Comments / doctype / PIs: skip entirely.
      if (i + 1 < len && (html[i + 1] == '!' || html[i + 1] == '?')) {
        size_t j = i + 1;
        if (j + 2 < len && html[j] == '!' && html[j + 1] == '-' && html[j + 2] == '-') {
          const char* end = (const char*)memmem(html + j, len - j, "-->", 3);
          i = end ? (size_t)(end - html) + 3 : len;
        } else {
          while (j < len && html[j] != '>') j++;
          i = j < len ? j + 1 : len;
        }
        continue;
      }
      i = next;
      const TagRule* rule = findRule(tagName);
      if (!rule) continue;  // stripped tag, content flows through

      switch (rule->kind) {
        case TagKind::PageBreak:
          if (b.chapterHasContent()) b.flushChapter();
          break;
        case TagKind::Void:
          if (!closing) {
            b.chapterBuf += "<";
            b.chapterBuf += rule->rename ? rule->rename : tagName.c_str();
            b.chapterBuf += "/>";
          }
          break;
        case TagKind::Image: {
          if (closing) break;
          uint32_t recindex = 0;
          for (const auto& a : attrs) {
            if (a.name == "recindex" || a.name == "src") {
              recindex = (uint32_t)strtoul(a.value.c_str(), nullptr, 10);
              if (recindex) break;
            }
          }
          if (recindex && recindex <= 0xFFFF) {
            const auto* im = b.ensureImage((uint16_t)recindex);
            if (!im) {
              LOG_ERR("MOBI", "image recindex %lu unavailable (missing record or non-raster)",
                      (unsigned long)recindex);
            }
            if (im) {
              b.chapterBuf += "<img src=\"";
              b.chapterBuf += im->name;
              b.chapterBuf += "\" alt=\"\"/>";
            }
          }
          break;
        }
        case TagKind::Anchor:
          // Links stripped (filepos targets don't survive conversion); text kept.
          break;
        case TagKind::Inline:
        case TagKind::Block: {
          const char* outName = rule->rename ? rule->rename : tagName.c_str();
          if (!closing) {
            // Oversized chapter: split before opening a new block.
            if (isBlockKind(rule->kind) && b.chapterBuf.size() > CHAPTER_SOFT_LIMIT && b.openTags.empty()) {
              b.flushChapter();
            } else if (b.chapterBuf.size() > CHAPTER_HARD_LIMIT) {
              b.forceSplit();
            }
            // Auto-close a dangling <p> when a new block opens (HTML allows
            // implicit </p>, XHTML doesn't).
            if (isBlockKind(rule->kind) && !b.openTags.empty() && b.openTags.back() == "p" &&
                strcmp(outName, "p") != 0 && strcmp(outName, "span") != 0) {
              b.chapterBuf += "</p>";
              b.openTags.pop_back();
            }
            b.chapterBuf += "<";
            b.chapterBuf += outName;
            // Alignment is the one presentational attr the reader honors.
            for (const auto& a : attrs) {
              if (a.name == "align" &&
                  (a.value == "center" || a.value == "right" || a.value == "left" || a.value == "justify")) {
                b.chapterBuf += " style=\"text-align:";
                emitAttrValue(b.chapterBuf, a.value);
                b.chapterBuf += "\"";
                break;
              }
            }
            b.chapterBuf += ">";
            b.openTags.push_back(outName);
            if (outName[0] == 'h' && outName[1] >= '1' && outName[1] <= '6' && b.pendingHeadingText.empty()) {
              b.inHeading = true;
            }
          } else {
            // Close: unwind to the matching tag if present; ignore stray closers.
            int found = -1;
            for (int k = (int)b.openTags.size() - 1; k >= 0; k--) {
              if (b.openTags[k] == outName) {
                found = k;
                break;
              }
            }
            if (found >= 0) {
              while ((int)b.openTags.size() > found) {
                b.chapterBuf += "</" + b.openTags.back() + ">";
                b.openTags.pop_back();
              }
            }
            if (outName[0] == 'h') b.inHeading = false;
          }
          break;
        }
        case TagKind::Strip:
          break;
      }
      continue;
    }

    // Text content
    uint32_t cp = 0;
    if (c == '&') {
      // Entity
      size_t j = i + 1;
      if (j < len && html[j] == '#') {
        j++;
        uint32_t v = 0;
        if (j < len && (html[j] == 'x' || html[j] == 'X')) {
          j++;
          while (j < len && isxdigit((unsigned char)html[j])) {
            v = v * 16 + (isdigit((unsigned char)html[j]) ? html[j] - '0' : (tolower((unsigned char)html[j]) - 'a' + 10));
            j++;
          }
        } else {
          while (j < len && isdigit((unsigned char)html[j])) v = v * 10 + (html[j++] - '0');
        }
        if (j < len && html[j] == ';') {
          appendTextChar(b.chapterBuf, v ? v : 0xFFFD);
          i = j + 1;
        } else {
          b.chapterBuf += "&amp;";
          i++;
        }
        if (b.inHeading && b.pendingHeadingText.size() < 64) b.pendingHeadingText += '?';
        continue;
      }
      size_t nameLen = 0;
      while (j + nameLen < len && nameLen < 12 && isalnum((unsigned char)html[j + nameLen])) nameLen++;
      if (j + nameLen < len && html[j + nameLen] == ';' && nameLen > 0) {
        const int ecp = lookupEntity(html + j, nameLen);
        if (ecp >= 0) {
          appendTextChar(b.chapterBuf, (uint32_t)ecp);
          if (b.inHeading && b.pendingHeadingText.size() < 64 && ecp < 128 && ecp >= 32)
            b.pendingHeadingText += (char)ecp;
        } else {
          b.chapterBuf += "&amp;";
          b.chapterBuf.append(html + j, nameLen + 1);
        }
        i = j + nameLen + 1;
        continue;
      }
      b.chapterBuf += "&amp;";
      i++;
      continue;
    }

    if (b.chapterBuf.size() > CHAPTER_HARD_LIMIT) b.forceSplit();

    const uint8_t uc = (uint8_t)c;
    (void)cp;
    if (uc < 0x80) {
      appendTextChar(b.chapterBuf, uc);
      if (b.inHeading && b.pendingHeadingText.size() < 64 && uc >= 32) b.pendingHeadingText += c;
    } else if (mobi.isUtf8()) {
      // Already UTF-8: pass the byte through untouched (ASCII specials were
      // escaped above; continuation bytes are never '<' or '&').
      b.chapterBuf += c;
    } else {
      // cp1252 -> UTF-8
      appendUtf8(b.chapterBuf, uc < 0xA0 ? CP1252_HIGH[uc - 0x80] : uc);
    }
    i++;
  }

  if (b.chapterHasContent() || b.chapterCount == 0) b.flushChapter();
  if (b.failed) {
    setError("EPUB write failed (SD full?)");
    b.zip.abort();  // SdFat must not hold the file open across remove()
    Storage.remove(epubPath.c_str());
    return "";
  }

  // 3) Manifest + spine + TOC.
  std::string opf;
  opf.reserve(4096);
  opf +=
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
      "<package xmlns=\"http://www.idpf.org/2007/opf\" unique-identifier=\"bookid\" version=\"2.0\">\n"
      "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:opf=\"http://www.idpf.org/2007/opf\">\n";
  opf += "<dc:title>";
  {
    std::string t;
    for (char ch : mobi.title()) appendTextChar(t, (uint8_t)ch < 0x80 ? (uint8_t)ch : 0xFFFD);
    opf += t;
  }
  opf += "</dc:title>\n<dc:creator>";
  {
    std::string a;
    for (char ch : mobi.author()) appendTextChar(a, (uint8_t)ch < 0x80 ? (uint8_t)ch : 0xFFFD);
    opf += a;
  }
  opf += "</dc:creator>\n<dc:language>en</dc:language>\n<dc:identifier id=\"bookid\">mobi-";
  {
    char hex[12];
    snprintf(hex, sizeof(hex), "%08x", (unsigned)hash);
    opf += hex;
  }
  opf += "</dc:identifier>\n";
  // Cover metadata (EXTH 201): reference the cover image if the book has one
  // and it was extracted.
  if (mobi.coverRecindex() > 0) {
    const auto* cover = b.ensureImage((uint16_t)mobi.coverRecindex());
    if (cover) {
      char meta[64];
      snprintf(meta, sizeof(meta), "<meta name=\"cover\" content=\"img%05u\"/>\n", (unsigned)mobi.coverRecindex());
      opf += meta;
    }
  }
  opf += "</metadata>\n<manifest>\n";
  opf += "<item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>\n";
  for (int c2 = 0; c2 < b.chapterCount; c2++) {
    char line[128];
    snprintf(line, sizeof(line),
             "<item id=\"ch%04d\" href=\"chapter%04d.xhtml\" media-type=\"application/xhtml+xml\"/>\n", c2, c2);
    opf += line;
  }
  for (const auto& im : b.images) {
    opf += "<item id=\"img";
    char n[16];
    snprintf(n, sizeof(n), "%05u", im.recindex);
    opf += n;
    opf += "\" href=\"" + im.name + "\" media-type=\"" + im.media + "\"/>\n";
  }
  opf += "</manifest>\n<spine toc=\"ncx\">\n";
  for (int c2 = 0; c2 < b.chapterCount; c2++) {
    char line[64];
    snprintf(line, sizeof(line), "<itemref idref=\"ch%04d\"/>\n", c2);
    opf += line;
  }
  opf += "</spine>\n</package>\n";
  if (!b.zip.addFile("OEBPS/content.opf", (const uint8_t*)opf.data(), opf.size())) {
    setError("EPUB write failed");
    b.zip.abort();
    Storage.remove(epubPath.c_str());
    return "";
  }

  std::string ncx;
  ncx.reserve(2048);
  ncx +=
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
      "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\">\n<head/>\n<docTitle><text>";
  ncx += "book";
  ncx += "</text></docTitle>\n<navMap>\n";
  for (int c2 = 0; c2 < b.chapterCount; c2++) {
    char line[256];
    std::string label = b.chapterTitles[(size_t)c2];
    if (label.empty()) {
      char lbl[32];
      snprintf(lbl, sizeof(lbl), "Chapter %d", c2 + 1);
      label = lbl;
    }
    std::string esc;
    for (char ch : label) appendTextChar(esc, (uint8_t)ch);
    snprintf(line, sizeof(line),
             "<navPoint id=\"n%04d\" playOrder=\"%d\"><navLabel><text>%s</text></navLabel>"
             "<content src=\"chapter%04d.xhtml\"/></navPoint>\n",
             c2, c2 + 1, esc.c_str(), c2);
    ncx += line;
  }
  ncx += "</navMap>\n</ncx>\n";
  if (!b.zip.addFile("OEBPS/toc.ncx", (const uint8_t*)ncx.data(), ncx.size()) || !b.zip.finish()) {
    setError("EPUB write failed");
    b.zip.abort();
    Storage.remove(epubPath.c_str());
    return "";
  }

  // 4) Cache tag.
  {
    HalFile f;
    if (Storage.openFileForWrite("MOBI", tagPath.c_str(), f)) {
      uint32_t tag[2] = {CONVERTER_VERSION, mobi.rawTextLength()};
      f.write(tag, sizeof(tag));
    }
  }
  LOG_INF("MOBI", "converted: %d chapters, %u images", b.chapterCount, (unsigned)b.images.size());
  return epubPath;
}
