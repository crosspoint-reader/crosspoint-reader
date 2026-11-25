#include "EpubBuilder.h"

#include <cstring>

namespace readwise {

uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t len) {
  static uint32_t table[256];
  static bool ready = false;
  if (!ready) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    ready = true;
  }
  crc = ~crc;
  for (size_t i = 0; i < len; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

std::string xmlEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (const char c : s) {
    switch (c) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      case '"':
        out += "&quot;";
        break;
      default:
        // Control characters are not allowed in XML 1.0.
        if (static_cast<unsigned char>(c) >= 0x20 || c == '\t' || c == '\n') out += c;
        break;
    }
  }
  return out;
}

namespace {

constexpr const char* kContainer =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
    "<rootfiles><rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/>"
    "</rootfiles>\n</container>\n";

constexpr const char* kStyle =
    "body{margin:0}\n"
    "h1{font-size:1.4em;margin:0 0 .3em 0;text-align:left;text-indent:0}\n"
    "p.rw-byline{font-style:italic;font-size:.85em;margin:0 0 1.2em 0;text-indent:0;text-align:left}\n"
    "blockquote{margin:.5em 1em;font-style:italic}\n"
    "pre,code{font-family:monospace}\n";

void put16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
void put32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

std::string byline(const ArticleMeta& m) {
  std::string s = m.author;
  if (!m.siteName.empty() && m.siteName != m.author) {
    if (!s.empty()) s += " \xC2\xB7 ";  // middle dot
    s += m.siteName;
  }
  return s;
}

}  // namespace

std::string EpubBuilder::chapterHead(const ArticleMeta& meta) {
  const std::string title = xmlEscape(meta.title.empty() ? "Untitled" : meta.title);
  std::string s =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<!DOCTYPE html>\n"
      "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\" lang=\"" +
      xmlEscape(meta.language) + "\" xml:lang=\"" + xmlEscape(meta.language) +
      "\">\n<head>\n<meta charset=\"UTF-8\"/>\n<title>" + title +
      "</title>\n<link rel=\"stylesheet\" type=\"text/css\" href=\"style.css\"/>\n</head>\n<body>\n<h1>" + title +
      "</h1>\n";
  const std::string by = byline(meta);
  if (!by.empty()) s += "<p class=\"rw-byline\">" + xmlEscape(by) + "</p>\n";
  return s;
}

std::string EpubBuilder::chapterTail() { return "\n</body>\n</html>\n"; }

bool EpubBuilder::raw(const void* data, size_t len) {
  if (len == 0) return true;
  if (!write_(static_cast<const uint8_t*>(data), len)) return false;
  offset_ += static_cast<uint32_t>(len);
  return true;
}

bool EpubBuilder::beginEntry(const char* name, uint32_t crc, uint32_t size) {
  if (entryCount_ >= sizeof(entries_) / sizeof(entries_[0])) return false;
  entries_[entryCount_++] = Entry{name, crc, size, offset_};
  const uint16_t nameLen = static_cast<uint16_t>(std::strlen(name));
  uint8_t h[30] = {};
  put32(h, 0x04034b50);
  put16(h + 4, 10);       // version needed: 1.0 (stored)
  put16(h + 6, 0x0800);   // flags: UTF-8 names
  put16(h + 8, 0);        // method: stored
  put16(h + 10, 0);       // mod time
  put16(h + 12, 0x5B21);  // mod date: 2025-09-01
  put32(h + 14, crc);
  put32(h + 18, size);
  put32(h + 22, size);
  put16(h + 26, nameLen);
  put16(h + 28, 0);
  return raw(h, sizeof(h)) && raw(name, nameLen);
}

bool EpubBuilder::addEntry(const char* name, const std::string& data) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(data.data());
  return beginEntry(name, crc32Update(0, bytes, data.size()), static_cast<uint32_t>(data.size())) &&
         raw(bytes, data.size());
}

bool EpubBuilder::finish() {
  const uint32_t cdStart = offset_;
  for (uint8_t i = 0; i < entryCount_; ++i) {
    const Entry& e = entries_[i];
    const uint16_t nameLen = static_cast<uint16_t>(std::strlen(e.name));
    uint8_t h[46] = {};
    put32(h, 0x02014b50);
    put16(h + 4, 0x031E);  // made by: Unix, 3.0
    put16(h + 6, 10);
    put16(h + 8, 0x0800);
    put16(h + 10, 0);
    put16(h + 12, 0);
    put16(h + 14, 0x5B21);
    put32(h + 16, e.crc);
    put32(h + 20, e.size);
    put32(h + 24, e.size);
    put16(h + 28, nameLen);
    put32(h + 38, 0644u << 16);  // external attrs: -rw-r--r--
    put32(h + 42, e.offset);
    if (!raw(h, sizeof(h)) || !raw(e.name, nameLen)) return false;
  }
  const uint32_t cdSize = offset_ - cdStart;
  uint8_t end[22] = {};
  put32(end, 0x06054b50);
  put16(end + 8, entryCount_);
  put16(end + 10, entryCount_);
  put32(end + 12, cdSize);
  put32(end + 16, cdStart);
  return raw(end, sizeof(end));
}

bool EpubBuilder::writeArticle(const ArticleMeta& meta, const uint32_t bodySize, const CopyBody& copyBody) {
  const std::string title = xmlEscape(meta.title.empty() ? "Untitled" : meta.title);
  const std::string lang = xmlEscape(meta.language.empty() ? "en" : meta.language);
  const std::string uid = "readwise:" + xmlEscape(meta.id);

  std::string opf =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"uid\" xml:lang=\"" +
      lang +
      "\">\n<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
      "<dc:identifier id=\"uid\">" +
      uid + "</dc:identifier>\n<dc:title>" + title + "</dc:title>\n";
  if (!meta.author.empty()) opf += "<dc:creator>" + xmlEscape(meta.author) + "</dc:creator>\n";
  if (!meta.siteName.empty()) opf += "<dc:publisher>" + xmlEscape(meta.siteName) + "</dc:publisher>\n";
  if (!meta.sourceUrl.empty()) opf += "<dc:source>" + xmlEscape(meta.sourceUrl) + "</dc:source>\n";
  opf += "<dc:language>" + lang +
         "</dc:language>\n<meta property=\"dcterms:modified\">2025-01-01T00:00:00Z</meta>\n</metadata>\n"
         "<manifest>\n"
         "<item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>\n"
         "<item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>\n"
         "<item id=\"css\" href=\"style.css\" media-type=\"text/css\"/>\n"
         "<item id=\"article\" href=\"article.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
         "</manifest>\n<spine toc=\"ncx\">\n<itemref idref=\"article\"/>\n</spine>\n</package>\n";

  const std::string nav =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE html>\n"
      "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\">\n"
      "<head><title>" +
      title + "</title></head>\n<body>\n<nav epub:type=\"toc\" id=\"toc\"><ol><li><a href=\"article.xhtml\">" + title +
      "</a></li></ol></nav>\n</body>\n</html>\n";

  const std::string ncx =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" version=\"2005-1\">\n"
      "<head><meta name=\"dtb:uid\" content=\"" +
      uid + "\"/></head>\n<docTitle><text>" + title +
      "</text></docTitle>\n<navMap><navPoint id=\"p1\" playOrder=\"1\"><navLabel><text>" + title +
      "</text></navLabel><content src=\"article.xhtml\"/></navPoint></navMap>\n</ncx>\n";

  const std::string head = chapterHead(meta);
  const std::string tail = chapterTail();

  // Pass 1: CRC over head + body + tail (the body is re-read in pass 2).
  uint32_t crc = crc32Update(0, reinterpret_cast<const uint8_t*>(head.data()), head.size());
  uint32_t measured = 0;
  const Write crcPass = [&crc, &measured](const uint8_t* d, size_t n) {
    crc = crc32Update(crc, d, n);
    measured += static_cast<uint32_t>(n);
    return true;
  };
  if (!copyBody(crcPass) || measured != bodySize) return false;
  crc = crc32Update(crc, reinterpret_cast<const uint8_t*>(tail.data()), tail.size());

  // mimetype must be first and stored.
  if (!addEntry("mimetype", "application/epub+zip")) return false;
  if (!addEntry("META-INF/container.xml", kContainer)) return false;
  if (!addEntry("OEBPS/content.opf", opf)) return false;
  if (!addEntry("OEBPS/nav.xhtml", nav)) return false;
  if (!addEntry("OEBPS/toc.ncx", ncx)) return false;
  if (!addEntry("OEBPS/style.css", kStyle)) return false;

  const uint32_t total = static_cast<uint32_t>(head.size()) + bodySize + static_cast<uint32_t>(tail.size());
  if (!beginEntry("OEBPS/article.xhtml", crc, total)) return false;
  if (!raw(head.data(), head.size())) return false;
  uint32_t copied = 0;
  const Write counted = [this, &copied](const uint8_t* d, size_t n) {
    copied += static_cast<uint32_t>(n);
    return raw(d, n);
  };
  if (!copyBody(counted) || copied != bodySize) return false;
  if (!raw(tail.data(), tail.size())) return false;
  return finish();
}

// --- language guess ---------------------------------------------------------

namespace {
constexpr const char* kLangs[] = {"en", "nl", "de", "fr", "es", "it", "pt"};
constexpr const char* kWords[][11] = {
    {"the", "and", "that", "with", "this", "have", "from", "which", "would", "there", "were"},
    {"het", "een", "niet", "zijn", "voor", "ook", "maar", "wordt", "deze", "naar", "worden"},
    {"der", "und", "nicht", "ist", "auch", "sich", "eine", "werden", "dass", "wird", "mit"},
    {"les", "est", "une", "dans", "pour", "pas", "qui", "sur", "avec", "sont", "cette"},
    {"los", "las", "por", "para", "una", "como", "pero", "del", "sus", "este", "esta"},
    {"il", "che", "della", "non", "sono", "gli", "anche", "nel", "questo", "delle", "alla"},
    {"uma", "com", "dos", "das", "mais", "pelo", "isso", "pela", "seu", "sua", "ao"},
};
}  // namespace

void LanguageGuesser::feed(const char* data, size_t len) {
  for (size_t i = 0; i < len && words_ < kSampleWords; ++i) {
    const char c = data[i];
    if (inTag_) {
      if (c == '>') inTag_ = false;
      continue;
    }
    if (c == '<') {
      endWord();
      inTag_ = true;
      continue;
    }
    const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || static_cast<unsigned char>(c) >= 0x80;
    if (!letter) {
      endWord();
      continue;
    }
    if (wordLen_ < kWordMax) {
      word_[wordLen_++] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    } else {
      wordTooLong_ = true;
    }
  }
}

void LanguageGuesser::endWord() {
  if (wordLen_ == 0) return;
  if (!wordTooLong_) {
    word_[wordLen_] = '\0';
    ++words_;
    for (size_t l = 0; l < sizeof(kLangs) / sizeof(kLangs[0]); ++l) {
      for (const char* w : kWords[l]) {
        if (std::strcmp(w, word_) == 0) {
          ++scores_[l];
          break;
        }
      }
    }
  }
  wordLen_ = 0;
  wordTooLong_ = false;
}

const char* LanguageGuesser::result() const {
  size_t best = 0;
  for (size_t l = 1; l < sizeof(kLangs) / sizeof(kLangs[0]); ++l) {
    if (scores_[l] > scores_[best]) best = l;
  }
  return scores_[best] >= 8 ? kLangs[best] : "en";
}

}  // namespace readwise
