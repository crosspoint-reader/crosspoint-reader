#include "FolderWalk.h"

#include <HalStorage.h>
#include <HalSystem.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdint>
#include <vector>

namespace FolderWalk {
namespace {

// A long file name is up to 255 UTF-16 units, so up to 765 bytes of UTF-8.
constexpr size_t NAME_BUFFER_BYTES = 768;
// Levels the walk holds before its stack of positions grows; a deeper tree still walks.
constexpr size_t RESERVED_LEVELS = 16;

void append(std::string& path, const char* name) {
  if (path.empty() || path.back() != '/') path += '/';
  path += name;
}

}  // namespace

bool eachFile(const std::string& dir, const FileVisitor visit, void* ctx) {
  {
    HalFile root = Storage.open(dir.c_str());
    if (!root || !root.isDirectory()) return false;
  }
  // Heap, not stack: the visitor runs below this frame.
  auto name = makeUniqueNoThrow<char[]>(NAME_BUFFER_BYTES);
  if (!name) {
    LOG_ERR("FWK", "OOM: %u byte name buffer for %s", static_cast<unsigned>(NAME_BUFFER_BYTES), dir.c_str());
    return false;
  }

  std::string path = dir;          // the folder being listed
  std::vector<uint32_t> resumeAt;  // for each folder above it, where its listing goes on
  resumeAt.reserve(RESERVED_LEVELS);
  uint32_t position = 0;  // where the listing of `path` starts
  while (true) {
    bool descended = false;
    HalFile folder = Storage.open(path.c_str());
    if (!folder || !folder.isDirectory() || !folder.seekSet(position)) {
      LOG_ERR("FWK", "Cannot list %s; what is below it is left out", path.c_str());
    } else {
      for (HalFile entry = folder.openNextFile(); entry; entry = folder.openNextFile()) {
        HalSystem::feedWatchdog();
        entry.getName(name.get(), NAME_BUFFER_BYTES);
        const bool isFolder = entry.isDirectory();
        entry.close();
        if (isFolder) {
          // This folder is closed below, and reopened here once the subfolder is done.
          resumeAt.push_back(static_cast<uint32_t>(folder.position()));
          append(path, name.get());
          position = 0;
          descended = true;
          break;
        }
        std::string file = path;
        append(file, name.get());
        visit(file, ctx);
      }
    }
    if (folder) folder.close();
    if (descended) continue;
    if (resumeAt.empty()) return true;
    const size_t parentEnd = path.rfind('/');
    path.resize(parentEnd == 0 ? 1 : parentEnd);  // a folder in "/" goes back to "/"
    position = resumeAt.back();
    resumeAt.pop_back();
  }
}

}  // namespace FolderWalk
