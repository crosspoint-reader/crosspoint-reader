#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace BidiUtils {

inline constexpr int RTL_DETECTION_SCAN_LETTERS = 5;

bool startsWithRtl(const char* utf8, int maxStrongChars = RTL_DETECTION_SCAN_LETTERS);

int detectParagraphLevel(const char* utf8, int fallbackLevel = 0, int maxStrongChars = 64);

std::string applyBidiVisual(const char* utf8, int paragraphLevel = -1);

bool computeVisualWordOrder(const std::vector<std::string>& words, bool paragraphIsRtl,
                            std::vector<uint16_t>& visualOrder);

}  // namespace BidiUtils
