#pragma once
// A web-novel-shaped EPUB: one spine item per chapter, an image per chapter, an NCX that lists
// every chapter. Shaped after King's Avatar (1732 spine items, ~1600 images interleaved with them),
// the book a cold first open of which sat 431 s under "Indexing" on the X3 (2026-10-20).
//
// The chapters sit in the archive a third of the book apart from their spine neighbours, each
// followed by an image: a lookup that resumes after the previous spine item's entry reads about a
// third of the central directory to find the next one. That is what King's Avatar's archive order
// does to ZipFile's lookup cursor (837906162: 200-1733 entries per lookup, ~246 ms each on the
// X3). STORED, so the archive is written without a deflater.
//
// Options shape the rest after the books that exist: a manifest in name order (Sigil writes one:
// chapter1, chapter10, chapter100, ... against a spine in reading order), and the package
// documents (OPF, NCX, stylesheets) behind every chapter and image in the archive, where a lookup
// by name reads the whole central directory to reach them.
#include <algorithm>
#include <cstdint>
#include <numeric>
#include <string>
#include <vector>

#include "StoredZipWriter.h"

namespace big_spine_book {

struct Book {
  std::string path;
  // Spine hrefs as the OPF parse records them (relative to the archive root), in spine order.
  std::vector<std::string> spineHrefs;
  // Each spine item's size in the archive, in spine order.
  std::vector<uint32_t> spineSizes;
  // Entries in the archive.
  size_t entries = 0;
};

inline std::string chapterName(const int i) { return "chapter" + std::to_string(i) + ".xhtml"; }
inline std::string imageName(const int i) { return "image" + std::to_string(i) + ".jpg"; }

// A chapter of a few hundred bytes to a KB and a half, different for every chapter so that every
// cumulative size in book.bin depends on the right entry having been found.
inline std::string chapterDocument(const int i) {
  std::string text;
  const int words = 40 + (i * 37) % 200;
  for (int w = 0; w < words; ++w) text += "word" + std::to_string((i + w) % 97) + " ";
  return "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>C" +
         std::to_string(i) +
         "</title><link rel=\"stylesheet\" href=\"../Styles/style.css\"/></head><body>\n<h1>Chapter " +
         std::to_string(i) + "</h1><p>" + text + "</p><p><img src=\"../Images/" + imageName(i) +
         "\" alt=\"\"/></p>\n</body></html>\n";
}

// Not a decodable JPEG: indexing only sizes and names it.
inline std::string imageBytes(const int i) {
  return std::string("\xFF\xD8\xFF\xE0", 4) + std::string(static_cast<size_t>(60 + i % 50), 'j');
}

struct Options {
  bool manifestInNameOrder = false;
  bool documentsLast = false;
  int stylesheets = 1;
  int rulesPerStylesheet = 40;
};

inline std::string stylesheetName(const int s) { return s == 0 ? "style.css" : "style" + std::to_string(s) + ".css"; }

// A stylesheet of a few dozen rules, the kind a converter writes.
inline std::string stylesheet(const int s, const int rules) {
  std::string css = "body { margin: 0; } h1 { font-weight: bold; }\n";
  for (int r = 0; r < rules; ++r) {
    css += ".c" + std::to_string(s) + "_" + std::to_string(r) + " { margin-left: " + std::to_string(r % 7) +
           "em; text-indent: 1em; font-style: italic; }\n";
  }
  return css;
}

inline Book write(const std::string& path, const int chapters, const int images, const Options& options = {}) {
  Book book;
  book.path = path;

  std::string manifest =
      "<item id=\"ncx\" href=\"toc.ncx\" media-type=\"application/x-dtbncx+xml\"/>\n"
      "<item id=\"cover\" href=\"Images/cover.jpg\" media-type=\"image/jpeg\" properties=\"cover-image\"/>\n";
  for (int s = 0; s < options.stylesheets; ++s) {
    manifest += "<item id=\"css" + std::to_string(s) + "\" href=\"Styles/" + stylesheetName(s) +
                "\" media-type=\"text/css\"/>\n";
  }
  std::vector<int> manifestOrder(static_cast<size_t>(chapters));
  std::iota(manifestOrder.begin(), manifestOrder.end(), 0);
  if (options.manifestInNameOrder) {
    std::sort(manifestOrder.begin(), manifestOrder.end(),
              [](const int a, const int b) { return chapterName(a) < chapterName(b); });
  }
  for (const int i : manifestOrder) {
    manifest += "<item id=\"c" + std::to_string(i) + "\" href=\"Text/" + chapterName(i) +
                "\" media-type=\"application/xhtml+xml\"/>\n";
  }
  std::string spine;
  std::string navPoints;
  for (int i = 0; i < chapters; ++i) {
    const std::string id = "c" + std::to_string(i);
    spine += "<itemref idref=\"" + id + "\"/>\n";
    navPoints += "<navPoint id=\"n" + std::to_string(i) + "\" playOrder=\"" + std::to_string(i + 1) +
                 "\"><navLabel><text>Chapter " + std::to_string(i) + "</text></navLabel><content src=\"Text/" +
                 chapterName(i) + "\"/></navPoint>\n";
  }
  for (int i = 0; i < images; ++i) {
    manifest +=
        "<item id=\"i" + std::to_string(i) + "\" href=\"Images/" + imageName(i) + "\" media-type=\"image/jpeg\"/>\n";
  }

  struct Entry {
    std::string name;
    std::string data;
  };
  std::vector<Entry> entries;
  std::vector<Entry> documents;
  entries.push_back({"META-INF/container.xml",
                     "<?xml version=\"1.0\"?>\n<container version=\"1.0\" "
                     "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n<rootfiles><rootfile "
                     "full-path=\"OEBPS/content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles>\n"
                     "</container>\n"});
  documents.push_back({"OEBPS/content.opf",
                       "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<package xmlns=\"http://www.idpf.org/2007/opf\" "
                       "version=\"2.0\" unique-identifier=\"id\">\n<metadata "
                       "xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:identifier id=\"id\">big</dc:identifier>"
                       "<dc:title>Big Spine</dc:title><dc:creator>Test</dc:creator><dc:language>en</dc:language>"
                       "<meta name=\"cover\" content=\"cover\"/></metadata>\n<manifest>\n" +
                           manifest + "</manifest>\n<spine toc=\"ncx\">\n" + spine + "</spine>\n</package>\n"});
  documents.push_back(
      {"OEBPS/toc.ncx",
       "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<ncx xmlns=\"http://www.daisy.org/z3986/2005/ncx/\" "
       "version=\"2005-1\"><head></head><docTitle><text>Big Spine</text></docTitle><navMap>\n" +
           navPoints + "</navMap></ncx>\n"});
  for (int s = 0; s < options.stylesheets; ++s) {
    documents.push_back({"OEBPS/Styles/" + stylesheetName(s), stylesheet(s, options.rulesPerStylesheet)});
  }
  if (!options.documentsLast) entries.insert(entries.end(), documents.begin(), documents.end());
  entries.push_back({"OEBPS/Images/cover.jpg", imageBytes(0)});
  book.spineSizes.reserve(static_cast<size_t>(chapters));
  for (int i = 0; i < chapters; ++i) {
    book.spineHrefs.push_back("OEBPS/Text/" + chapterName(i));
    book.spineSizes.push_back(static_cast<uint32_t>(chapterDocument(i).size()));
  }
  // Chapter i goes to archive slot (i * stride) mod chapters: spine neighbours a stride apart. The
  // stride is the first one from a third of the book up that visits every slot.
  int stride = chapters / 3 > 0 ? chapters / 3 : 1;
  while (std::gcd(stride, chapters) != 1) ++stride;
  std::vector<int> chapterInSlot(static_cast<size_t>(chapters));
  for (int i = 0; i < chapters; ++i) {
    chapterInSlot[static_cast<size_t>((static_cast<int64_t>(i) * stride) % chapters)] = i;
  }
  for (int k = 0; k < chapters || k < images; ++k) {
    if (k < chapters) {
      const int i = chapterInSlot[static_cast<size_t>(k)];
      entries.push_back({"OEBPS/Text/" + chapterName(i), chapterDocument(i)});
    }
    if (k < images) entries.push_back({"OEBPS/Images/" + imageName(k), imageBytes(k)});
  }
  if (options.documentsLast) entries.insert(entries.end(), documents.begin(), documents.end());

  test_zip::StoredZipWriter zip;
  zip.add("mimetype", "application/epub+zip");  // first, as the spec requires
  for (const Entry& e : entries) zip.add(e.name, e.data);
  zip.write(path);
  book.entries = entries.size() + 1;
  return book;
}

}  // namespace big_spine_book
