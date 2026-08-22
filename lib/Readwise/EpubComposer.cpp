#include "EpubComposer.h"

#include <Logging.h>

#include <cstdio>
#include <cstring>

namespace {
// Copies src into dst XML-escaped (&, <, >, "), never splitting a UTF-8
// sequence at the truncation boundary.
void xmlEscapeCopy(const char* src, char* dst, size_t dstSize) {
  size_t o = 0;
  for (size_t i = 0; src[i] != '\0' && o + 1 < dstSize; i++) {
    const char c = src[i];
    const char* rep = nullptr;
    if (c == '&') {
      rep = "&amp;";
    } else if (c == '<') {
      rep = "&lt;";
    } else if (c == '>') {
      rep = "&gt;";
    } else if (c == '"') {
      rep = "&quot;";
    }
    if (rep) {
      const size_t rl = strlen(rep);
      if (o + rl >= dstSize) break;
      memcpy(dst + o, rep, rl);
      o += rl;
    } else {
      dst[o++] = c;
    }
  }
  // Back off to a UTF-8 lead byte so a truncated multibyte char stays valid.
  while (o > 0 && (static_cast<uint8_t>(dst[o - 1]) & 0xC0) == 0x80) o--;
  dst[o] = '\0';
}

bool writeChunk(ZipStreamWriter& zip, const char* s) { return zip.write(reinterpret_cast<const uint8_t*>(s), strlen(s)); }

const char* CONTAINER_XML =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n"
    "  <rootfiles>\n"
    "    <rootfile full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/>\n"
    "  </rootfiles>\n"
    "</container>\n";

const char* CONTENT_HEAD =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<!DOCTYPE html>\n"
    "<html xmlns=\"http://www.w3.org/1999/xhtml\">\n"
    "<head><title/></head>\n"
    "<body>\n";

const char* CONTENT_TAIL = "\n</body>\n</html>\n";
}  // namespace

EpubComposer::EpubComposer(const char* sdPath) : html_({this, &EpubComposer::sinkWrite}) {
  snprintf(path_, sizeof(path_), "%s", sdPath);
}

int EpubComposer::sinkWrite(void* ctx, const char* data, const size_t len) {
  auto* self = static_cast<EpubComposer*>(ctx);
  return self->zip_.write(reinterpret_cast<const uint8_t*>(data), len) ? 0 : 1;
}

bool EpubComposer::begin() {
  if (!Storage.ensureDirectoryExists("/Readwise")) {
    LOG_ERR("RWISE", "Cannot create /Readwise");
    return false;
  }
  file_.close();
  if (!Storage.openFileForWrite("RWISE", path_, file_)) {
    LOG_ERR("RWISE", "Cannot open %s", path_);
    return false;
  }
  if (!zip_.begin(file_) || !zip_.beginEntry("mimetype") ||
      !writeChunk(zip_, "application/epub+zip") || !zip_.finishEntry()) {
    LOG_ERR("RWISE", "mimetype entry failed");
    return false;
  }
  if (!zip_.beginEntry("META-INF/container.xml") || !writeChunk(zip_, CONTAINER_XML) || !zip_.finishEntry()) {
    LOG_ERR("RWISE", "container.xml entry failed");
    return false;
  }
  return true;
}

bool EpubComposer::beginContent() {
  if (!zip_.beginEntry("OEBPS/content.xhtml")) return false;
  if (!writeChunk(zip_, CONTENT_HEAD)) return false;
  html_.reset();
  return true;
}

bool EpubComposer::contentChunk(const char* data, const size_t len) {
  html_.feed(data, len);
  return !html_.hasError();
}

bool EpubComposer::endContent(const char* title, const char* author, const char* docId) {
  html_.finish();
  if (!writeChunk(zip_, CONTENT_TAIL) || !zip_.finishEntry()) {
    return false;
  }

  char escTitle[256];
  char escAuthor[128];
  xmlEscapeCopy(title, escTitle, sizeof(escTitle));
  xmlEscapeCopy(author, escAuthor, sizeof(escAuthor));

  if (!zip_.beginEntry("OEBPS/content.opf")) return false;
  char buf[128];
  snprintf(buf, sizeof(buf),
           "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
           "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\" unique-identifier=\"bookid\">\n"
           "  <metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
           "    <dc:identifier id=\"bookid\">urn:readwise:%s</dc:identifier>\n",
           docId);
  if (!writeChunk(zip_, buf)) return false;
  // Title/author are emitted as separate writes so an over-long escaped value
  // can never splice the template string mid-buffer.
  if (!writeChunk(zip_, "    <dc:title>") || !writeChunk(zip_, escTitle) ||
      !writeChunk(zip_, "</dc:title>\n    <dc:creator>") || !writeChunk(zip_, escAuthor) ||
      !writeChunk(zip_,
                  "</dc:creator>\n"
                  "    <dc:language>en</dc:language>\n"
                  "    <meta property=\"dcterms:modified\">2026-01-01T00:00:00Z</meta>\n"
                  "  </metadata>\n")) {
    return false;
  }
  static constexpr const char* OPF_MANIFEST =
      "  <manifest>\n"
      "    <item id=\"content\" href=\"content.xhtml\" media-type=\"application/xhtml+xml\"/>\n"
      "    <item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>\n"
      "  </manifest>\n"
      "  <spine>\n"
      "    <itemref idref=\"content\"/>\n"
      "  </spine>\n"
      "</package>\n";
  if (!writeChunk(zip_, OPF_MANIFEST) || !zip_.finishEntry()) return false;

  if (!zip_.beginEntry("OEBPS/nav.xhtml")) return false;
  static constexpr const char* NAV_HEAD =
      "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
      "<!DOCTYPE html>\n"
      "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\">\n"
      "<head><title/></head>\n"
      "<body>\n"
      "  <nav epub:type=\"toc\">\n"
      "    <ol>\n";
  if (!writeChunk(zip_, NAV_HEAD)) return false;
  if (!writeChunk(zip_, "      <li><a href=\"content.xhtml\">") || !writeChunk(zip_, escTitle) ||
      !writeChunk(zip_, "</a></li>\n")) {
    return false;
  }
  if (!writeChunk(zip_, "    </ol>\n  </nav>\n</body>\n</html>\n") || !zip_.finishEntry()) return false;

  if (!zip_.finish()) return false;
  file_.flush();
  file_.close();
  return true;
}

void EpubComposer::abort() {
  // Explicit close before remove(): DESTRUCTOR_CLOSES_FILE would otherwise
  // run after the delete.
  file_.close();
  Storage.remove(path_);
}
