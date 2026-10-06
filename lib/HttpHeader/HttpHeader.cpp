#include "HttpHeader.h"

#include <AsciiText.h>

void parseHttpHeaderLine(const std::string_view line, HttpHeader& header) {
  const size_t colon = line.find(':');
  header.name = trimAsciiWhitespace(line.substr(0, colon));
  if (colon == std::string_view::npos) {
    header.value.clear();
  } else {
    header.value = trimAsciiWhitespace(line.substr(colon + 1));
  }
}

std::string formatHttpHeaderLine(const HttpHeader& header, const bool maskValue) {
  if (header.name.empty()) return "";
  if (header.value.empty()) return header.name;
  return header.name + ": " + (maskValue ? "******" : header.value);
}
