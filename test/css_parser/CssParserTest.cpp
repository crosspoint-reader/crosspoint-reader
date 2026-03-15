#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

#include "../../lib/Epub/Epub/css/CssParser.h"

namespace {

void testInlineDirectionParsing() {
  const CssStyle style = CssParser::parseInlineStyle("direction: rtl; text-align: right;");
  assert(style.hasDirection());
  assert(style.direction == CssDirection::Rtl);
  assert(style.hasTextAlign());
  assert(style.textAlign == CssTextAlign::Right);
}

void testSelectorCascadeWithDirection() {
  const std::filesystem::path cssPath = std::filesystem::temp_directory_path() / "crosspoint_css_direction.css";
  {
    std::ofstream css(cssPath);
    css << "p { direction: ltr; }\n";
    css << ".rtl { direction: rtl; }\n";
    css << "p.override { direction: ltr; text-align: left; }\n";
  }

  FsFile file;
  assert(Storage.openFileForRead("CSS", cssPath.string(), file));

  CssParser parser("");
  assert(parser.loadFromStream(file));
  file.close();

  CssStyle style = parser.resolveStyle("p", "rtl");
  assert(style.hasDirection());
  assert(style.direction == CssDirection::Rtl);

  style = parser.resolveStyle("p", "rtl override");
  assert(style.hasDirection());
  assert(style.direction == CssDirection::Ltr);
  assert(style.hasTextAlign());
  assert(style.textAlign == CssTextAlign::Left);

  std::filesystem::remove(cssPath);
}

void testCacheRoundTripPreservesDirection() {
  const std::filesystem::path cacheDir = std::filesystem::temp_directory_path() / "crosspoint_css_cache_test";
  std::filesystem::create_directories(cacheDir);

  const std::filesystem::path cssPath = cacheDir / "style.css";
  {
    std::ofstream css(cssPath);
    css << ".rtl { direction: rtl; }\n";
  }

  FsFile file;
  assert(Storage.openFileForRead("CSS", cssPath.string(), file));

  CssParser writer(cacheDir.string());
  assert(writer.loadFromStream(file));
  file.close();
  assert(writer.saveToCache());

  CssParser reader(cacheDir.string());
  assert(reader.loadFromCache());
  const CssStyle style = reader.resolveStyle("div", "rtl");
  assert(style.hasDirection());
  assert(style.direction == CssDirection::Rtl);

  std::filesystem::remove_all(cacheDir);
}

}  // namespace

int main() {
  testInlineDirectionParsing();
  testSelectorCascadeWithDirection();
  testCacheRoundTripPreservesDirection();
  return 0;
}
