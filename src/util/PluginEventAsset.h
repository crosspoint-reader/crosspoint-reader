#pragma once

#include <string>

namespace plugineventasset {

// Require exact dimensions; (0, 0) accepts any supported size for backup recovery.
bool validBmp(const std::string& path, int width, int height);

// Resolve a leftover .bak; false means validation, cleanup or restore failed.
bool recover(const std::string& dest);

// Replaces dest with a checked .part file; false leaves the old bytes at dest
// or at dest.bak for recovery on the next drain.
bool commit(const std::string& dest, int width, int height, int status);

}  // namespace plugineventasset
