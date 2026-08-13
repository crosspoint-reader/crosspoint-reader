#pragma once

#include <string>
#include <vector>

namespace opf_test {
// Host-test observation point: records the hrefs the parser resolves from spine
// <itemref> entries so tests can assert on the resulting spine.
std::vector<std::string>& spineHrefs();
}  // namespace opf_test
