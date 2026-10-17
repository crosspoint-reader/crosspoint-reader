#pragma once

#include <string>
#include <string_view>

// A single extra header sent with a request, on top of whatever auth/UA
// headers the transport already sets (e.g. a Cloudflare Access service token).
struct HttpHeader {
  std::string name;
  std::string value;
};

// "Name: Value" -> trimmed name/value. A missing colon treats the whole
// entry as a bare header name with no value.
void parseHttpHeaderLine(std::string_view line, HttpHeader& header);

// Inverse of parseHttpHeaderLine(); empty when the header is unset. With
// maskValue, a non-empty value is replaced by "******" for display.
std::string formatHttpHeaderLine(const HttpHeader& header, bool maskValue = false);
