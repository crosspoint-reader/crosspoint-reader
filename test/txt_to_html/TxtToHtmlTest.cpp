#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "Print.h"
#include "TxtToHtml.h"

namespace {

class StringPrint : public Print {
 public:
  std::string str;
  size_t write(uint8_t b) override {
    str.push_back(static_cast<char>(b));
    return 1;
  }
  size_t write(const uint8_t* buffer, size_t size) override {
    str.append(reinterpret_cast<const char*>(buffer), size);
    return size;
  }
};

const std::string kHeader =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<!-- TXT_CACHE_VERSION: 1 -->\n"
    "<!DOCTYPE html>\n<html>\n<head><title>test</title></head>\n<body>\n";
const std::string kFooter = "\n</body>\n</html>\n";

std::string convert(std::string_view content) {
  StringPrint out;
  EXPECT_TRUE(TxtToHtml::stream("test.txt", content, out));
  return out.str;
}

TEST(TxtToHtmlTest, LeadingSpacesIndentation) {
  EXPECT_EQ(convert("  two spaces"), kHeader + "&#160;&#160;two spaces" + kFooter);
  EXPECT_EQ(convert("    four spaces"), kHeader + "&#160;&#160;&#160;&#160;four spaces" + kFooter);
  EXPECT_EQ(convert("Line 1\n   three spaces"), kHeader + "Line 1<br />&#160;&#160;&#160;three spaces" + kFooter);
}

TEST(TxtToHtmlTest, MidLineConsecutiveSpaces) {
  EXPECT_EQ(convert("One space"), kHeader + "One space" + kFooter);
  EXPECT_EQ(convert("Two  spaces"), kHeader + "Two&#160; spaces" + kFooter);
  EXPECT_EQ(convert("Three   spaces"), kHeader + "Three&#160;&#160; spaces" + kFooter);
  EXPECT_EQ(convert("Four    spaces"), kHeader + "Four&#160;&#160;&#160; spaces" + kFooter);
}

TEST(TxtToHtmlTest, PreservesCacheVersionTagsForBothFormats) {
  StringPrint out;
  ASSERT_TRUE(TxtToHtml::stream("test.MD", "text", out));
  EXPECT_EQ(out.str,
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<!-- MD_CACHE_VERSION: 1 -->\n"
            "<!DOCTYPE html>\n<html>\n<head><title>test</title></head>\n<body>\ntext" +
                kFooter);
  EXPECT_NE(out.str.find(TxtToHtml::cacheVersionTag("test.MD")), std::string::npos);
  EXPECT_NE(convert("text").find(TxtToHtml::cacheVersionTag("test.txt")), std::string::npos);
}

}  // namespace
