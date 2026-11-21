#pragma once

#include "Epub.h"

struct Xtc : Epub {
  Xtc(const std::string& path, const std::string& cacheDir) : Epub(path, cacheDir, "/xtc_") {}
};
