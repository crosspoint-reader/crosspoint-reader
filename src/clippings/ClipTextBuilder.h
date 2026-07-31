#pragma once

#include <vector>

#include "../activities/ActivityResult.h"
#include "../activities/reader/WordRef.h"

/// Builds a ClippingResult from a confirmed word selection.
/// Separated from ClipSelectionActivity so callers can also invoke it independently
/// (e.g. post-processing after a WORD_SELECT result, or unit testing).
namespace ClipTextBuilder {

/// Build a full ClippingResult from [from, to] indices in words.
/// Handles em-space stripping, hyphen merging, paragraph newlines,
/// and start/end/mid/context anchor extraction.
ClippingResult build(const std::vector<WordRef>& words, int from, int to, int total, int startPageInSection);

}  // namespace ClipTextBuilder
