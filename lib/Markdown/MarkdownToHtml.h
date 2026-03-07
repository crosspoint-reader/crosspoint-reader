#pragma once

#include <HalStorage.h>

#include <string>

class MarkdownToHtml {
 public:
  // Convert a markdown file to HTML, writing to outputPath.
  // Streaming: reads in chunks, processes line-by-line, minimal RAM.
  // Returns true on success.
  static bool convert(const std::string& mdPath, const std::string& htmlPath);
};
