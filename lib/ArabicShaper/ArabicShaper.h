#pragma once

#include <cstdint>
#include <vector>

namespace ArabicShaper {

void shape(const char* text, std::vector<uint32_t>& output);
std::vector<uint32_t> shape(const char* text);

}  // namespace ArabicShaper