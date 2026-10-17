#pragma once

#include <string>

namespace clippingSync {
// Disabled in KOSync settings by default. Returns false on an enabled sync failure.
bool run(const std::string& bookPath, const std::string& documentHash);
}  // namespace clippingSync
