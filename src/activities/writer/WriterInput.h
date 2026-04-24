#pragma once

#include <string>

namespace WriterInput {
void setActive(bool active);
bool readAvailable(std::string& out);
}  // namespace WriterInput
