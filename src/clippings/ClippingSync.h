#pragma once

#include <string>

namespace clippingSync {
// Disabled in KOSync settings by default. Returns false on an enabled sync failure.
// C3 requires an active framebuffer loan with rendering locked until this returns.
bool run(const std::string& bookPath, const std::string& documentHash);
}  // namespace clippingSync
