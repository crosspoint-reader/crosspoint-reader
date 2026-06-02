#include <ChapterXPathResolver.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace {
std::string testItemHref;
std::string testXhtml;

constexpr const char* DIRECT_BODY_TEXT_CHAPTER_XHTML = R"xml(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml">
<head><title>Direct Body Text</title></head>
<body>Intro<div>Middle</div>tail</body>
</html>)xml";

constexpr const char* MIXED_INLINE_CHAPTER_XHTML = R"xml(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml">
<head><title>Mixed Inline Content</title></head>
<body><div>Lead <span>Alpha</span> beta</div></body>
</html>)xml";

constexpr const char* PARAGRAPH_CHAPTER_XHTML = R"xml(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml">
<head><title>Paragraph Content</title></head>
<body>
<h1>Paragraph Content</h1>
<p>Alpha paragraph begins the small fixture.</p>
<p>Bravo paragraph contains enough words to be selected near the middle of the file.</p>
<p>Charlie paragraph closes the chapter.</p>
</body>
</html>)xml";

constexpr const char* DIV_CHAPTER_XHTML = R"xml(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml">
<head><title>Div Content</title></head>
<body>
<h1>Div Content</h1>
<div class="body"><span>Alpha div begins</span> the small fixture.</div>
<div class="body">Bravo div contains enough words to be selected near the middle of the file.</div>
<div class="body">Charlie div closes the chapter.</div>
</body>
</html>)xml";

void loadFixtureChapter(const char* itemHref, const char* xhtml) {
  testItemHref = itemHref;
  testXhtml = xhtml;
}
}  // namespace

int Epub::getSpineItemsCount() const { return 3; }
Epub::SpineEntry Epub::getSpineItem(int spineIndex) const {
  if (spineIndex == 0) {
    return Epub::SpineEntry{"chapter1.xhtml"};
  }
  if (spineIndex == 1) {
    return Epub::SpineEntry{"chapter2.xhtml"};
  }
  if (spineIndex == 2) {
    return Epub::SpineEntry{"chapter3.xhtml"};
  }
  return {};
}
bool Epub::readItemContentsToStream(const std::string& itemHref, Print& out, size_t) const {
  if (itemHref != testItemHref) {
    return false;
  }
  return out.write(reinterpret_cast<const uint8_t*>(testXhtml.data()), testXhtml.size()) == testXhtml.size();
}

TEST(ChapterXPathResolver, ResolvesDirectBodyText) {
  loadFixtureChapter("chapter1.xhtml", DIRECT_BODY_TEXT_CHAPTER_XHTML);

  const std::string xpath = ChapterXPathResolver::findXPathForProgress(std::make_shared<Epub>(), 0, 0.10f);

  EXPECT_EQ(xpath, "/body/DocFragment[1]/body/text()[1].2");
}

TEST(ChapterXPathResolver, ResolvesPostInlineTextNode) {
  loadFixtureChapter("chapter1.xhtml", MIXED_INLINE_CHAPTER_XHTML);

  const std::string xpath = ChapterXPathResolver::findXPathForProgress(std::make_shared<Epub>(), 0, 0.80f);

  EXPECT_EQ(xpath, "/body/DocFragment[1]/body/div[1]/text()[2].2");
}

TEST(ChapterXPathResolver, ResolvesParagraphChapterText) {
  loadFixtureChapter("chapter2.xhtml", PARAGRAPH_CHAPTER_XHTML);

  const std::string xpath = ChapterXPathResolver::findXPathForProgress(std::make_shared<Epub>(), 1, 0.55f);

  EXPECT_EQ(xpath, "/body/DocFragment[2]/body/p[2]/text()[1].39");
}

TEST(ChapterXPathResolver, KeepsZeroProgressAtChapterBody) {
  loadFixtureChapter("chapter2.xhtml", PARAGRAPH_CHAPTER_XHTML);

  EXPECT_EQ(ChapterXPathResolver::findXPathForProgress(std::make_shared<Epub>(), 1, 0.0f), "/body/DocFragment[2]/body");
}

TEST(ChapterXPathResolver, ResolvesDivOnlyChapterText) {
  loadFixtureChapter("chapter3.xhtml", DIV_CHAPTER_XHTML);

  const std::string xpath = ChapterXPathResolver::findXPathForProgress(std::make_shared<Epub>(), 2, 0.55f);

  EXPECT_EQ(xpath, "/body/DocFragment[3]/body/div[2]/text()[1].38");
}
