"""Run with python3 test/test_sleep_image_pool.py (requires a host C++ compiler)."""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "src/activities/boot_sleep/SleepActivity.cpp").read_text()
marker = "bool selectRandomSleepFile("
selection_function = marker + source.split(marker, 1)[1].split("bool drawSleepPopupPreservingFrame", 1)[0]

harness = r"""
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "activities/boot_sleep/SleepImagePool.h"

#define LOG_ERR(...)
#define random fakeRandom

constexpr size_t MAX_SLEEP_FILE_NAME_LEN = 256;
enum class SleepRecentKind : uint8_t { Standard, Overlay };

template <typename T>
std::unique_ptr<T> makeUniqueNoThrow(size_t count) {
  using Element = std::remove_extent_t<T>;
  return std::unique_ptr<T>(new Element[count]);
}

struct FakeDir {
  std::vector<std::string>* files = nullptr;
  size_t position = 0;

  explicit operator bool() const { return files != nullptr; }
  bool isDirectory() const { return files != nullptr; }
};

struct FakeStorage {
  std::map<std::string, std::vector<std::string>> dirs;

  FakeDir open(const char* path) {
    auto it = dirs.find(path);
    if (it == dirs.end()) return {};
    return {&it->second, 0};
  }
} Storage;

struct FakeState {
  uint8_t recentSleepFill = 0;
  uint8_t recentOverlaySleepFill = 0;
  int saves = 0;

  void saveToFile() { saves++; }
} APP_STATE;

std::vector<uint16_t> recentStandard;
std::vector<uint16_t> recentOverlay;
std::vector<uint16_t> randomSequence;
size_t randomPosition = 0;

bool findNextValidSleepImage(FakeDir& dir, SleepRecentKind, char* name) {
  if (!dir.files || dir.position >= dir.files->size()) return false;
  const auto& value = (*dir.files)[dir.position++];
  std::strncpy(name, value.c_str(), MAX_SLEEP_FILE_NAME_LEN - 1);
  name[MAX_SLEEP_FILE_NAME_LEN - 1] = '\0';
  return true;
}

bool isRecentSleepIndex(const SleepRecentKind kind, const uint16_t idx, const uint8_t window) {
  const auto& recent = kind == SleepRecentKind::Overlay ? recentOverlay : recentStandard;
  const size_t count = std::min<size_t>(window, recent.size());
  for (size_t i = 0; i < count; i++) {
    if (recent[recent.size() - 1 - i] == idx) return true;
  }
  return false;
}

void pushRecentSleepIndex(const SleepRecentKind kind, const uint16_t idx) {
  auto& recent = kind == SleepRecentKind::Overlay ? recentOverlay : recentStandard;
  recent.push_back(idx);
  if (kind == SleepRecentKind::Overlay) {
    APP_STATE.recentOverlaySleepFill = static_cast<uint8_t>(std::min<size_t>(16, recent.size()));
  } else {
    APP_STATE.recentSleepFill = static_cast<uint8_t>(std::min<size_t>(16, recent.size()));
  }
}

long fakeRandom(const long upper) {
  assert(upper > 0);
  assert(randomPosition < randomSequence.size());
  return randomSequence[randomPosition++] % upper;
}

void resetHarness() {
  Storage.dirs.clear();
  APP_STATE = {};
  recentStandard.clear();
  recentOverlay.clear();
  randomSequence.clear();
  randomPosition = 0;
}
""" + selection_function + r"""

int main() {
  {
    const uint16_t counts[] = {2, 3};
    auto first = SleepImagePool::resolveIndex(counts, 2, 1);
    assert(first.valid && first.directoryIndex == 0 && first.localIndex == 1);
    auto second = SleepImagePool::resolveIndex(counts, 2, 4);
    assert(second.valid && second.directoryIndex == 1 && second.localIndex == 2);
    assert(!SleepImagePool::resolveIndex(counts, 2, 5).valid);
  }

  resetHarness();
  Storage.dirs["/.sleep"] = {"a.bmp", "b.bmp"};
  randomSequence = {1};
  std::string path;
  assert(selectRandomSleepFile("/.sleep", "/sleep", SleepRecentKind::Standard, path));
  assert(path == "/.sleep/b.bmp");
  assert(recentStandard.back() == 1 && APP_STATE.saves == 1);

  resetHarness();
  Storage.dirs["/sleep"] = {"c.bmp", "d.bmp"};
  randomSequence = {1};
  path.clear();
  assert(selectRandomSleepFile("/.sleep", "/sleep", SleepRecentKind::Standard, path));
  assert(path == "/sleep/d.bmp");
  assert(recentStandard.back() == 1);

  resetHarness();
  Storage.dirs["/.sleep"] = {"a.bmp", "b.bmp"};
  Storage.dirs["/sleep"] = {"c.bmp", "d.bmp", "e.bmp"};
  randomSequence = {4};
  path.clear();
  assert(selectRandomSleepFile("/.sleep", "/sleep", SleepRecentKind::Standard, path));
  assert(path == "/sleep/e.bmp");
  assert(recentStandard.back() == 4);

  resetHarness();
  Storage.dirs["/.sleep"] = {"hidden.bmp"};
  Storage.dirs["/sleep"] = {"visible.bmp"};
  recentStandard = {0};
  APP_STATE.recentSleepFill = 1;
  randomSequence = {0, 1};
  path.clear();
  assert(selectRandomSleepFile("/.sleep", "/sleep", SleepRecentKind::Standard, path));
  assert(path == "/sleep/visible.bmp");
  assert(randomPosition == 2 && recentStandard.back() == 1);

  resetHarness();
  Storage.dirs["/.sleep-overlay"] = {"hidden.png"};
  Storage.dirs["/sleep-overlay"] = {"legacy.png"};
  recentOverlay = {0};
  APP_STATE.recentOverlaySleepFill = 1;
  randomSequence = {0, 1};
  path.clear();
  assert(selectRandomSleepFile("/.sleep-overlay", "/sleep-overlay", SleepRecentKind::Overlay, path));
  assert(path == "/sleep-overlay/legacy.png");
  assert(randomPosition == 2 && recentOverlay.back() == 1);

  resetHarness();
  path.clear();
  assert(!selectRandomSleepFile("/.sleep", "/sleep", SleepRecentKind::Standard, path));
  assert(path.empty() && APP_STATE.saves == 0);
}
"""

with tempfile.TemporaryDirectory() as directory:
    cpp = Path(directory) / "sleep_image_pool.cpp"
    executable = Path(directory) / "sleep_image_pool"
    cpp.write_text(harness)
    subprocess.run(
        ["c++", "-std=c++20", "-I", str(ROOT / "src"), str(cpp), "-o", str(executable)],
        check=True,
    )
    subprocess.run([str(executable)], check=True)

print("Sleep image pool regression checks passed")
