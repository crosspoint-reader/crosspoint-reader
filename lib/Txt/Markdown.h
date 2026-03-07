#pragma once

#include "Txt.h"

class Markdown : public Txt {
 public:
  Markdown(const std::string& path, const std::string& cacheBasePath) : Txt(path, cacheBasePath, "md_") {}
};
