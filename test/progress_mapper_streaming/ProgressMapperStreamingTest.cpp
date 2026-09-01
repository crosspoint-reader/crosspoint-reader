#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "Epub.h"
#include "GfxRenderer.h"
#include "ProgressMapper.h"

namespace {
std::shared_ptr<Epub> makeBook(const std::string& source, std::size_t fragmentSize) {
  auto epub = std::make_shared<Epub>();
  epub->fragmentSize = fragmentSize;
  epub->spine.push_back({"chapter.xhtml", source.size()});
  epub->contents.push_back(source);
  return epub;
}

CrossPointPosition resolve(const std::string& source, const std::string& xpath, std::size_t fragmentSize = 1024,
                           float percentage = 0.5f) {
  GfxRenderer renderer;
  return ProgressMapper::toCrossPoint(makeBook(source, fragmentSize), {xpath, percentage}, renderer);
}

TEST(ProgressMapperStreaming, CommentAndPiAreTerminatedByTheirFullDelimiters) {
  const std::string source =
      "<html><body><p>ab<!-- embedded > and -- not enough -->cd<?pi > data ?>ef</p></body></html>";
  for (const std::size_t fragmentSize : {source.size(), std::size_t{1}}) {
    const auto commentTarget = resolve(source, "/body/DocFragment[1]/body/p[1]/text()[2].1", fragmentSize);
    const auto piTarget = resolve(source, "/body/DocFragment[1]/body/p[1]/text()[3].1", fragmentSize);
    EXPECT_TRUE(commentTarget.hasVisibleTextOffset);
    EXPECT_EQ(commentTarget.visibleTextOffset, 3u);
    EXPECT_TRUE(piTarget.hasVisibleTextOffset);
    EXPECT_EQ(piTarget.visibleTextOffset, 5u);
  }
}

TEST(ProgressMapperStreaming, QuotedAndUnquotedAttributeSlashesDoNotCloseElementsEarly) {
  const std::string source =
      "<html><body><p>before<a id=\"anchor/part\" title=\"quoted > value\" href=scheme://host/path>link</a>"
      "after</p></body></html>";
  const auto result = resolve(source, "/body/DocFragment[1]/body/p[1]/text()[2].1", 1);
  EXPECT_TRUE(result.hasVisibleTextOffset);
  EXPECT_EQ(result.visibleTextOffset, 11u);
  EXPECT_STREQ(result.xpathAnchorId, "anchor/part");
}

TEST(ProgressMapperStreaming, EmptyConstructsBeforeTextDoNotCreatePhantomNodes) {
  const std::string source = "<html><body><p><!-- comment --><?pi ?><![CDATA[]]>text</p></body></html>";
  for (const std::size_t fragmentSize : {source.size(), std::size_t{1}}) {
    const auto first = resolve(source, "/body/DocFragment[1]/body/p[1]/text()[1].1", fragmentSize);
    EXPECT_TRUE(first.hasVisibleTextOffset);
    EXPECT_EQ(first.visibleTextOffset, 1u);
    const auto phantom = resolve(source, "/body/DocFragment[1]/body/p[1]/text()[2].1", fragmentSize);
    EXPECT_FALSE(phantom.hasVisibleTextOffset);
  }
}

TEST(ProgressMapperStreaming, VisibleCdataIsCountedButHiddenCdataIsIgnored) {
  const std::string source =
      "<html><head><![CDATA[hidden head]]></head><body><style><![CDATA[hidden style]]></style>"
      "<p>ab<![CDATA[cd]]>ef</p></body></html>";
  for (const std::size_t fragmentSize : {source.size(), std::size_t{1}}) {
    const auto result = resolve(source, "/body/DocFragment[1]/body/p[1]/text()[2].1", fragmentSize);
    EXPECT_TRUE(result.hasVisibleTextOffset);
    EXPECT_EQ(result.visibleTextOffset, 3u);
    const auto thirdNode = resolve(source, "/body/DocFragment[1]/body/p[1]/text()[3].1", fragmentSize);
    EXPECT_TRUE(thirdNode.hasVisibleTextOffset);
    EXPECT_EQ(thirdNode.visibleTextOffset, 5u);
  }
}

TEST(ProgressMapperStreaming, DoctypeSubsetIgnoresQuotedGtAndNestedComments) {
  const std::string source =
      "<!DOCTYPE html [<!ENTITY sample \"quoted > value <!-- still quoted -->\"><!-- nested > comment -->]>"
      "<html><body><p>hello</p>"
      "</body></html>";
  for (const std::size_t fragmentSize : {source.size(), std::size_t{1}}) {
    const auto result = resolve(source, "/body/DocFragment[1]/body/p[1]/text()[1].3", fragmentSize);
    EXPECT_TRUE(result.hasVisibleTextOffset);
    EXPECT_EQ(result.visibleTextOffset, 3u);
  }
}

TEST(ProgressMapperStreaming, NamespacePrefixesMatchLocalNames) {
  const std::string source = "<x:html><x:body><x:p>hello</x:p></x:body></x:html>";
  const auto result = resolve(source, "/body/DocFragment[1]/body/ns:p[1]/text()[1].2", 1);
  EXPECT_TRUE(result.hasVisibleTextOffset);
  EXPECT_EQ(result.visibleTextOffset, 2u);
}

TEST(ProgressMapperStreaming, LongNamespacePrefixesDoNotOverflowLocalNames) {
  const std::string source =
      "<prefixThatIsMuchLongerThanTheTagBuffer:html><prefixThatIsMuchLongerThanTheTagBuffer:body>"
      "<prefixThatIsMuchLongerThanTheTagBuffer:p>hello</prefixThatIsMuchLongerThanTheTagBuffer:p>"
      "</prefixThatIsMuchLongerThanTheTagBuffer:body></prefixThatIsMuchLongerThanTheTagBuffer:html>";
  for (const std::size_t fragmentSize : {source.size(), std::size_t{1}}) {
    const auto result =
        resolve(source, "/body/DocFragment[1]/body/xpathPrefixThatIsAlsoLong:p[1]/text()[1].2", fragmentSize);
    EXPECT_TRUE(result.hasVisibleTextOffset);
    EXPECT_EQ(result.visibleTextOffset, 2u);
  }
}

TEST(ProgressMapperStreaming, LongLocalNamesDoNotMatchOrCorruptDepth) {
  for (const std::string& source :
       {std::string("<html><body><longcustomtag>wrong</longcustomtag><div>right</div></body></html>"),
        std::string("<html><body><longcustomtag/>wrong<div>right</div></body></html>")}) {
    for (const std::size_t fragmentSize : {source.size(), std::size_t{1}}) {
      const auto result = resolve(source, "/body/DocFragment[1]/body/div[1]/text()[1].1", fragmentSize);
      EXPECT_TRUE(result.hasVisibleTextOffset);
      EXPECT_EQ(result.visibleTextOffset, 6u);
    }
  }
}

TEST(ProgressMapperStreaming, QuotedSelfClosingChildrenAdvanceTextNodes) {
  for (const std::string& source :
       {std::string("<html><body><p>before<span title=\"quoted\"/>after</p></body></html>"),
        std::string("<html><body><p>before<span title='quoted'/>after</p></body></html>")}) {
    for (const std::size_t fragmentSize : {source.size(), std::size_t{1}}) {
      const auto result = resolve(source, "/body/DocFragment[1]/body/p[1]/text()[2].1", fragmentSize);
      EXPECT_TRUE(result.hasVisibleTextOffset);
      EXPECT_EQ(result.visibleTextOffset, 7u);
    }
  }
}

TEST(ProgressMapperStreaming, BrAndEntitiesUtf8AndCrlfRetainExistingCounting) {
  const std::string source = "<html><body><p>A&amp;\r\nB<br/>C<br />\xC3\xA9</p></body></html>";
  const auto result = resolve(source, "/body/DocFragment[1]/body/p[1]/text()[3].1", 1);
  EXPECT_TRUE(result.hasVisibleTextOffset);
  EXPECT_EQ(result.visibleTextOffset, 6u);
}

TEST(ProgressMapperStreaming, UnterminatedSpecialConstructsUsePercentageFallback) {
  const std::string comment = "<html><body><p>before<!-- unterminated";
  const std::string pi = "<html><body><p>before<?unterminated";
  const std::string cdata = "<html><body><p>before<![CDATA[unterminated";
  const std::string declaration = "<!DOCTYPE html [<!ENTITY x \"unterminated><html><body><p>after";
  for (const auto& source : {comment, pi, cdata, declaration}) {
    for (const std::size_t fragmentSize : {source.size(), std::size_t{1}}) {
      const auto result = resolve(source, "/body/DocFragment[1]/body/p[1]/text()[1].1", fragmentSize);
      EXPECT_FALSE(result.hasVisibleTextOffset) << source;
      EXPECT_EQ(result.spineIndex, 0);
      EXPECT_EQ(result.totalPages, 8);
      EXPECT_GE(result.pageNumber, 0);
      EXPECT_LT(result.pageNumber, result.totalPages);
    }
  }
}

TEST(ProgressMapperStreaming, EmptyTagNamesUsePercentageFallback) {
  const std::string source = "<html><body><p>before</>after</p></body></html>";
  for (const std::size_t fragmentSize : {source.size(), std::size_t{1}}) {
    const auto result = resolve(source, "/body/DocFragment[1]/body/p[1]/text()[1].1", fragmentSize);
    EXPECT_FALSE(result.hasVisibleTextOffset);
    EXPECT_EQ(result.totalPages, 8);
    EXPECT_GE(result.pageNumber, 0);
    EXPECT_LT(result.pageNumber, result.totalPages);
  }
}

TEST(ProgressMapperStreaming, WhitespaceAfterLtUsesPercentageFallback) {
  const std::string source = "<html><body><p>before< span>after</span></p></body></html>";
  for (const std::size_t fragmentSize : {source.size(), std::size_t{1}}) {
    const auto result = resolve(source, "/body/DocFragment[1]/body/p[1]/text()[1].1", fragmentSize);
    EXPECT_FALSE(result.hasVisibleTextOffset);
    EXPECT_EQ(result.totalPages, 8);
    EXPECT_GE(result.pageNumber, 0);
    EXPECT_LT(result.pageNumber, result.totalPages);
  }
}

TEST(ProgressMapperStreaming, UnclosedElementTreeUsesPercentageFallback) {
  const std::string source = "<html><body><p>text";
  for (const std::size_t fragmentSize : {source.size(), std::size_t{1}}) {
    const auto result = resolve(source, "/body/DocFragment[1]/body/p[1]/text()[1].1", fragmentSize);
    EXPECT_FALSE(result.hasVisibleTextOffset);
    EXPECT_EQ(result.totalPages, 8);
    EXPECT_GE(result.pageNumber, 0);
    EXPECT_LT(result.pageNumber, result.totalPages);
  }
}
}  // namespace
