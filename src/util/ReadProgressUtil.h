#pragma once

#include <string>

namespace ReadProgressUtil {

// Returns true if the book at `filepath` has been previously opened.
// Checks for the existence of a progress.bin file in the book's cache directory.
// Supports EPUB, TXT, MD, XTC, and XTCH file types.
bool hasBeenOpened(const std::string& filepath);

}  // namespace ReadProgressUtil
