#include "LibraryMeta.h"

#include <Logging.h>
#include <Memory.h>
#include <Print.h>
#include <ZipFile.h>

#include <algorithm>
#include <cstring>
#include <string_view>

namespace library {
namespace {

// Collects an inflated file into a string, refusing to grow past a cap.
//
// ZipFile streams into a Print, and nothing in the tree collects into memory with
// a bound. Unbounded would be a real hazard: uncompressedSize comes from the
// archive itself, so a malformed or hostile EPUB could otherwise ask for an
// arbitrary allocation on a device with tens of kilobytes to spare.
// Collects an inflated entry into ONE buffer allocated up front.
//
// Up front and nothrow, both deliberately. A std::string that grows re-allocates
// while still holding the old block, so filling 64 KB needed ~96 KB contiguous on
// a heap whose largest free block was 69 KB — and std::string allocates by
// throwing, which here means abort() and a reboot loop. That is a crash the
// maintainer hit on a real card. A single fixed buffer cannot fragment, and a
// failure to get it is a returned false rather than a dead device.
class BoundedSink final : public Print {
 public:
  explicit BoundedSink(const size_t cap) : cap(cap) { buffer = makeUniqueNoThrow<char[]>(cap); }

  bool ok() const { return buffer != nullptr; }

  size_t write(const uint8_t b) override { return write(&b, 1); }

  size_t write(const uint8_t* data, const size_t size) override {
    if (!buffer) return 0;
    const size_t room = cap > used ? cap - used : 0;
    const size_t take = std::min(size, room);
    if (take > 0) {
      memcpy(buffer.get() + used, data, take);
      used += take;
    }
    if (take < size) overflowed = true;
    // Everything wanted lives in <metadata>; the manifest that follows is why
    // some of these run to 44 KB. Stopping here is what makes 8 KB sufficient.
    if (!done && used >= kMetadataEnd.size()) {
      const std::string_view seen(buffer.get(), used);
      if (seen.find(kMetadataEnd) != std::string_view::npos) done = true;
    }
    return take;
  }

  std::string text() const { return buffer ? std::string(buffer.get(), used) : std::string(); }

  bool overflowed = false;
  bool done = false;

 private:
  static constexpr std::string_view kMetadataEnd{"</metadata>"};
  std::unique_ptr<char[]> buffer;
  size_t cap;
  size_t used = 0;
};

// Find `name` as an element, tolerating a namespace prefix ("dc:title") and
// attributes. Returns the offset just past the opening tag, or npos.
size_t findElementBody(const std::string& xml, const char* name, size_t from = 0) {
  const size_t nameLen = strlen(name);
  while (from < xml.size()) {
    const size_t lt = xml.find('<', from);
    if (lt == std::string::npos) return std::string::npos;

    size_t p = lt + 1;
    if (p < xml.size() && (xml[p] == '/' || xml[p] == '?' || xml[p] == '!')) {
      from = lt + 1;
      continue;
    }
    // Skip a namespace prefix.
    const size_t tagStart = p;
    const size_t colon = xml.find_first_of(":> \t\r\n/", tagStart);
    if (colon != std::string::npos && colon < xml.size() && xml[colon] == ':') p = colon + 1;

    if (xml.compare(p, nameLen, name) == 0) {
      const char after = p + nameLen < xml.size() ? xml[p + nameLen] : '\0';
      if (after == '>' || after == ' ' || after == '\t' || after == '\r' || after == '\n' || after == '/') {
        const size_t close = xml.find('>', p);
        if (close == std::string::npos) return std::string::npos;
        if (close > 0 && xml[close - 1] == '/') {  // self-closing: no body
          from = close + 1;
          continue;
        }
        return close + 1;
      }
    }
    from = lt + 1;
  }
  return std::string::npos;
}

std::string elementText(const std::string& xml, const char* name) {
  const size_t body = findElementBody(xml, name);
  if (body == std::string::npos) return {};
  const size_t end = xml.find('<', body);
  if (end == std::string::npos) return {};
  return xml.substr(body, end - body);
}

// The five predefined XML entities, plus the numeric forms publishers use for
// typographic apostrophes. Anything unrecognised is left as written rather than
// dropped, so an unknown entity costs a stray "&amp;" and not a lost title.
std::string decodeEntities(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size();) {
    if (in[i] != '&') {
      out.push_back(in[i++]);
      continue;
    }
    const size_t semi = in.find(';', i);
    if (semi == std::string::npos || semi - i > 10) {
      out.push_back(in[i++]);
      continue;
    }
    const std::string name = in.substr(i + 1, semi - i - 1);
    if (name == "amp") {
      out.push_back('&');
    } else if (name == "lt") {
      out.push_back('<');
    } else if (name == "gt") {
      out.push_back('>');
    } else if (name == "quot") {
      out.push_back('"');
    } else if (name == "apos" || name == "#39" || name == "#x27" || name == "#8217" || name == "#x2019") {
      out.push_back('\'');
    } else if (name == "#160" || name == "#xa0" || name == "nbsp") {
      out.push_back(' ');
    } else {
      out.append(in, i, semi - i + 1);
    }
    i = semi + 1;
  }
  return out;
}

std::string collapseWhitespace(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  bool space = false;
  for (const char c : in) {
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      space = true;
      continue;
    }
    if (space && !out.empty()) out.push_back(' ');
    space = false;
    out.push_back(c);
  }
  return out;
}

bool inflateBounded(ZipFile& zip, const char* entry, const size_t maxInflated, std::string& out, bool& tooLarge) {
  tooLarge = false;
  // No size pre-check. There used to be one, and it undid the whole point of the
  // early stop: a package document is often larger than the cap because of its
  // manifest, and rejecting it up front threw away a title and author that sit in
  // the first few hundred bytes. A real 44 KB document on the maintainer's card
  // was silently losing both.
  //
  // The sink is the bound. It never allocates more than the cap, stops at
  // </metadata>, and a header that lies about its size cannot make it allocate
  // more.

  BoundedSink sink(maxInflated);
  if (!sink.ok()) {
    LOG_ERR("LIBMETA", "no room for a %u-byte buffer", static_cast<unsigned>(maxInflated));
    return false;
  }
  // allowEarlyStop, so a 44 KB manifest is never inflated once the metadata block
  // has gone by.
  zip.readFileToStream(entry, sink, 1024, /*allowEarlyStop=*/true);
  out = sink.text();
  // Overflow is only a failure if the metadata block never arrived: past
  // </metadata> there is nothing left worth reading.
  if (sink.overflowed && !sink.done) {
    tooLarge = true;
    return false;
  }
  return !out.empty();
}

}  // namespace

std::string opfPathFromContainer(const std::string& containerXml) {
  // <rootfile full-path="OEBPS/content.opf" .../>. Scanned for rather than
  // parsed: the attribute is unambiguous, and a container.xml with anything else
  // wrong should still yield the path.
  const size_t attr = containerXml.find("full-path=");
  if (attr == std::string::npos) return {};
  const size_t quote = containerXml.find_first_of("\"'", attr);
  if (quote == std::string::npos) return {};
  const size_t end = containerXml.find(containerXml[quote], quote + 1);
  if (end == std::string::npos) return {};
  return decodeEntities(containerXml.substr(quote + 1, end - quote - 1));
}

void parseOpfMetadata(const std::string& opfXml, BookMetadata& out) {
  out.title = collapseWhitespace(decodeEntities(elementText(opfXml, "title")));
  out.author = collapseWhitespace(decodeEntities(elementText(opfXml, "creator")));
}

bool readBookMetadata(const std::string& epubPath, BookMetadata& out) {
  out = BookMetadata{};

  ZipFile zip(epubPath);
  if (!zip.open()) return false;

  std::string containerXml;
  bool tooLarge = false;
  if (!inflateBounded(zip, "META-INF/container.xml", LIBRARY_CONTAINER_MAX_INFLATED, containerXml, tooLarge)) {
    zip.close();
    return false;
  }

  const std::string opfPath = opfPathFromContainer(containerXml);
  if (opfPath.empty()) {
    zip.close();
    return false;
  }

  std::string opfXml;
  const bool ok = inflateBounded(zip, opfPath.c_str(), LIBRARY_OPF_MAX_INFLATED, opfXml, tooLarge);
  zip.close();
  out.opfTooLarge = tooLarge;
  if (!ok) return false;

  parseOpfMetadata(opfXml, out);
  return !out.title.empty() || !out.author.empty();
}

}  // namespace library
