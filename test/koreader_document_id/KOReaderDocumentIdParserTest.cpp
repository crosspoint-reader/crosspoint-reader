#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>

#include "OriginalDocumentIdParser.h"

namespace {
std::string parseInChunks(const std::string& xml, const size_t chunkSize) {
  OriginalDocumentIdParser parser(xml.size());
  if (!parser.setup()) {
    return {};
  }

  for (size_t offset = 0; offset < xml.size(); offset += chunkSize) {
    const size_t size = std::min(chunkSize, xml.size() - offset);
    if (parser.write(reinterpret_cast<const uint8_t*>(xml.data() + offset), size) != size) {
      return {};
    }
  }
  return parser.documentId;
}

TEST(KOReaderDocumentIdParser, ReadsOptimizerMetadata) {
  const std::string xml = R"(<?xml version="1.0"?>
    <package xmlns="http://www.idpf.org/2007/opf">
      <metadata>
        <meta name="crosspoint:original-koreader-document-id"
              content="0123456789abcdef0123456789abcdef"/>
      </metadata>
    </package>)";

  EXPECT_EQ(parseInChunks(xml, 17), "0123456789abcdef0123456789abcdef");
}

TEST(KOReaderDocumentIdParser, HandlesPrefixedElementsAndAttributeOrder) {
  const std::string xml = R"(<opf:package xmlns:opf="http://www.idpf.org/2007/opf">
    <opf:metadata>
      <opf:meta content="ABCDEF0123456789ABCDEF0123456789"
                name="crosspoint:original-koreader-document-id"/>
    </opf:metadata>
  </opf:package>)";

  EXPECT_EQ(parseInChunks(xml, 512), "abcdef0123456789abcdef0123456789");
}

TEST(KOReaderDocumentIdParser, IgnoresMetadataOutsidePackageMetadata) {
  const std::string xml = R"(<package>
    <manifest>
      <meta name="crosspoint:original-koreader-document-id"
            content="0123456789abcdef0123456789abcdef"/>
    </manifest>
  </package>)";

  EXPECT_TRUE(parseInChunks(xml, 23).empty());
}

TEST(KOReaderDocumentIdParser, RejectsMalformedDocumentIds) {
  for (const char* value : {
           "short",
           "0123456789abcdef0123456789abcdeg",
           " 0123456789abcdef0123456789abcdef",
       }) {
    const std::string xml =
        std::string(R"(<package><metadata><meta name="crosspoint:original-koreader-document-id" content=")") + value +
        R"("/></metadata></package>)";
    EXPECT_TRUE(parseInChunks(xml, 9).empty()) << value;
  }
}

TEST(KOReaderDocumentIdParser, IgnoresUnrelatedMetadata) {
  const std::string xml = R"(<package><metadata>
    <meta name="cover" content="cover-image"/>
    <meta name="crosspoint:something-else" content="0123456789abcdef0123456789abcdef"/>
  </metadata></package>)";

  EXPECT_TRUE(parseInChunks(xml, 64).empty());
}
}  // namespace
