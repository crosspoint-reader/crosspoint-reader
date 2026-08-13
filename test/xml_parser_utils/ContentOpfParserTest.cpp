#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "BookMetadataCacheStub.h"
#include "Epub/BookMetadataCache.h"
#include "Epub/parsers/ContentOpfParser.h"

namespace {

constexpr char kCachePath[] = "/.crosspoint/epub_test";
constexpr char kBaseContentPath[] = "OEBPS/";

// A complete content.opf whose package uses an arbitrary namespace prefix
// (ns0:) rather than the conventional "opf:" — the shape behind issue #2752,
// where the reader rendered "End of book" for a valid EPUB.
constexpr char kOpf[] = R"OPF(<?xml version="1.0" encoding="utf-8"?>
<ns0:package xmlns:ns0="http://www.idpf.org/2007/opf"
             xmlns:dc="http://purl.org/dc/elements/1.1/"
             version="2.0"
             unique-identifier="bookid">
  <ns0:metadata>
    <dc:title>Lorem Ipsum QName Test</dc:title>
    <dc:creator>CrossPoint Test</dc:creator>
    <dc:language>en</dc:language>
  </ns0:metadata>
  <ns0:manifest>
    <ns0:item id="cover" href="cover.xhtml" media-type="application/xhtml+xml"/>
    <ns0:item id="chapter1" href="chapter1.xhtml" media-type="application/xhtml+xml"/>
    <ns0:item id="chapter2" href="chapter2.xhtml" media-type="application/xhtml+xml"/>
    <ns0:item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>
  </ns0:manifest>
  <ns0:spine>
    <ns0:itemref idref="chapter1"/>
    <ns0:itemref idref="chapter2"/>
  </ns0:spine>
  <ns0:guide>
    <ns0:reference type="text" href="chapter1.xhtml" title="Beginning"/>
  </ns0:guide>
</ns0:package>
)OPF";

TEST(ContentOpfParser, ResolvesPrefixedSpineIntoNonEmptySpine) {
  opf_test::spineHrefs().clear();

  // ContentOpfParser stores these as const std::string& members, so they must
  // outlive the parser (a string literal would dangle).
  const std::string cachePath(kCachePath);
  const std::string baseContentPath(kBaseContentPath);

  BookMetadataCache cache(cachePath);

  const std::string opf(kOpf);
  ContentOpfParser parser(cachePath, baseContentPath, opf.size(), &cache);

  ASSERT_TRUE(parser.setup());
  ASSERT_EQ(parser.write(reinterpret_cast<const uint8_t*>(opf.data()), opf.size()), opf.size());

  EXPECT_EQ(parser.title, "Lorem Ipsum QName Test");
  EXPECT_GT(cache.getSpineCount(), 0);
  ASSERT_EQ(opf_test::spineHrefs().size(), 2u);
  EXPECT_EQ(opf_test::spineHrefs()[0], "OEBPS/chapter1.xhtml");
  EXPECT_EQ(opf_test::spineHrefs()[1], "OEBPS/chapter2.xhtml");
}

}  // namespace
