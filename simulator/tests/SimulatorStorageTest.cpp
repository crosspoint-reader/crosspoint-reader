#include "HalStorage.h"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;

namespace {

bool containsName(const std::vector<String> &names, const char *expected) {
  for (const String &name : names) {
    if (name == expected) return true;
  }
  return false;
}

size_t openDescriptorCount() {
  const fs::path descriptors("/proc/self/fd");
  if (!fs::exists(descriptors)) return 0;
  return static_cast<size_t>(
      std::distance(fs::directory_iterator(descriptors), fs::directory_iterator{}));
}

} // namespace

int main() {
  char temporary[] = "/tmp/crossvi-storage-test-XXXXXX";
  const char *created = ::mkdtemp(temporary);
  assert(created != nullptr);
  const fs::path base(created);
  const fs::path root = base / "sd";
  const fs::path outside = base / "outside";
  fs::create_directories(root);
  fs::create_directories(outside);
  assert(::setenv("CROSSVI_SIM_SD", root.c_str(), 1) == 0);
  assert(Storage.begin());
  assert(Storage.ready());

  assert(Storage.writeFile("/.hidden", String("visible to firmware")));
  assert(containsName(Storage.listFiles("/"), ".hidden"));

  bool hiddenSeen = false;
  {
    HalFile rootDirectory = Storage.open("/", O_RDONLY);
    assert(rootDirectory && rootDirectory.isDirectory());
    while (true) {
      HalFile entry = rootDirectory.openNextFile();
      if (!entry) break;
      char name[64] = {};
      assert(entry.getName(name, sizeof(name)) > 0);
      hiddenSeen = hiddenSeen || std::string(name) == ".hidden";
    }
  }
  assert(hiddenSeen);

  HalFile hidden = Storage.open("/.hidden", O_RDONLY);
  assert(hidden);
  char tooShort[4] = {'x', 'x', 'x', '\0'};
  assert(hidden.getName(tooShort, sizeof(tooShort)) == 0);
  assert(tooShort[0] == '\0');
  char fullName[16] = {};
  assert(hidden.getName(fullName, sizeof(fullName)) == 7);
  assert(std::string(fullName) == ".hidden");
  assert(hidden.getName(nullptr, 0) == 0);

  std::ofstream(outside / "keep.txt") << "must survive";
  fs::create_directory_symlink(outside, root / "escape");
  assert(!Storage.exists("/escape/keep.txt"));
  assert(!containsName(Storage.listFiles("/"), "escape"));
  assert(!Storage.removeDir("/escape"));
  assert(fs::exists(outside / "keep.txt"));

  const size_t descriptorsBefore = openDescriptorCount();
  for (int attempt = 0; attempt < 64; ++attempt) {
    HalFile directory = Storage.open("/", O_RDONLY);
    assert(directory && directory.isDirectory());
  }
  const size_t descriptorsAfter = openDescriptorCount();
  if (descriptorsBefore > 0) {
    assert(descriptorsAfter <= descriptorsBefore + 1);
  }

  assert(Storage.mkdir("/safe"));
  assert(Storage.writeFile("/safe/.cache", String("cache")));
  fs::create_directory_symlink(outside, root / "safe" / "outside-link");
  assert(Storage.removeDir("/safe"));
  assert(fs::exists(outside / "keep.txt"));

  hidden.close();
  fs::remove(root / "escape");
  fs::remove_all(base);
  return 0;
}
