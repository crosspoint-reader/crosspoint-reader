#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace SleepScreenCollection {

// The selected first-level directory is persisted in a fixed settings field.
// Longer names are skipped because truncating one would make it impossible to
// resolve the same directory after reboot.
static constexpr size_t MAX_SET_NAME_BYTES = 127;

// Bound discovery for predictable embedded RAM/UI use. The 16-option selector
// keeps the other two slots for Default and an unavailable saved selection.
static constexpr size_t MAX_NAMED_SET_COUNT = 14;

// Preserve the legacy directory priority. /sleep.bmp remains a separate,
// higher-priority check in SleepActivity.
const char* resolveDirectory();

// Scan exactly one organizational level: root BMPs form Default, and direct BMPs
// in each immediate child form that set. Never descending further keeps SD work
// and memory predictable and gives an embedded reader a simple folder model.
void discover(const char* sleepDirectory, std::vector<std::string>& sets);

// Resolve a safe immediate child directory. Missing or invalid names resolve to
// the root without changing the persisted selection. Returns true only when the
// named child directory exists.
bool resolveSelectedDirectory(const char* sleepDirectory, const char* selectedSet, std::string& selectedDirectory);

}  // namespace SleepScreenCollection
