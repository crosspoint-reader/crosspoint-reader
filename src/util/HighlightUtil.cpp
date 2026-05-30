#include "HighlightUtil.h"

#include <algorithm>
#include <cctype>
#include <string>

std::string HighlightUtil::getHighlightsDir() { return "/.crosspoint/highlights/"; }

std::string HighlightUtil::getHighlightPath(const std::string& bookPath) {
  // remove leading slash and replace internal slashes to create a flat filename
  std::string bookName = std::string(bookPath).erase(0, 1);
  std::replace(bookName.begin(), bookName.end(), '/', '_');
  std::replace(bookName.begin(), bookName.end(), '\\', '_');
  const size_t lastDot = bookName.find_last_of('.');
  if (lastDot != std::string::npos) {
    bookName.erase(lastDot);
  }
  bookName += ".json";
  return getHighlightsDir() + bookName;
}

std::string HighlightUtil::sanitizeHighlightText(std::string text) {
  // Collapse consecutive whitespace into a single character
  text.erase(std::unique(text.begin(), text.end(),
                         [](char a, char b) {
                           return std::isspace(static_cast<unsigned char>(a)) &&
                                  std::isspace(static_cast<unsigned char>(b));
                         }),
             text.end());
  // Drop newlines entirely
  text.erase(std::remove(text.begin(), text.end(), '\n'), text.end());
  // Trim leading whitespace
  text.erase(text.begin(), std::find_if(text.begin(), text.end(), [](unsigned char ch) { return !std::isspace(ch); }));
  // Trim trailing whitespace
  text.erase(std::find_if(text.rbegin(), text.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(),
             text.end());
  return text;
}
