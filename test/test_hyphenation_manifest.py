"""Run with python3 test/test_hyphenation_manifest.py after installing firmware dependencies."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "src/activities/settings/HyphenationManagerActivity.cpp").read_text()
header = (ROOT / "src/activities/settings/HyphenationManagerActivity.h").read_text()
# Compile the production allocator and loader with in-memory SD/HTTP stand-ins.
allocator = source.split("constexpr size_t MAX_MANIFEST_SIZE", 1)[1].split("bool isPackFilename", 1)[0]
allocator = "constexpr size_t MAX_MANIFEST_SIZE" + allocator
method = source.split("bool HyphenationManagerActivity::loadManifest()", 1)[1].split(
    "void HyphenationManagerActivity::onWifiReady", 1
)[0]
method = "bool HyphenationManagerActivity::loadManifest()" + method
pack = header.split("  struct Pack {", 1)[1].split("\n  };", 1)[0]
pack = "struct Pack {" + pack + "\n};"

harness = r"""
#include <ArduinoJson.h>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#define LOG_ERR(...) ((void)0)
static std::unordered_map<void*, size_t> blocks;
static size_t used = 0, peak = 0;
static void* trackedMalloc(size_t size) {
  void* ptr = std::malloc(size);
  if (ptr) { blocks[ptr] = size; used += size; peak = std::max(peak, used); }
  return ptr;
}
static void trackedFree(void* ptr) {
  if (!ptr) return;
  used -= blocks.at(ptr); blocks.erase(ptr); std::free(ptr);
}
""" + allocator.replace("std::malloc", "trackedMalloc").replace(
    "std::free", "trackedFree"
) + r"""
struct LanguageEntry { bool hyphenator = false; };
static const LanguageEntry english{true}, spanish{false};
static const LanguageEntry* findLanguageEntry(const char* code) {
  if (std::strcmp(code, "en") == 0) return &english;
  if (std::strcmp(code, "es") == 0) return &spanish;
  return nullptr;
}
struct Cache { void releaseSdFontCaches() {} };
struct Renderer { Cache* getFontCacheManager() { return nullptr; } };
struct Esp {
  size_t getFreeHeap() { return 100000; }
  size_t getMaxAllocHeap() { return 100000; }
} ESP;
struct HalFile {
  std::istringstream input;
  size_t length = 0;
  size_t size() { return length; }
  int read() { return input.get(); }
  size_t readBytes(char* data, size_t size) {
    input.read(data, size); return input.gcount();
  }
  void close() {}
};
struct Store {
  std::string fixture;
  void remove(const char*) {}
  bool openFileForRead(const char*, const char*, HalFile& file) {
    file.input.str(fixture); file.length = fixture.size(); return true;
  }
} Storage;
struct HttpDownloader {
  static constexpr size_t MIN_TLS_FREE_HEAP = 50000, MIN_TLS_MAX_ALLOC = 20000;
  static constexpr int OK = 0;
  static int downloadToFile(const char*, const char*, void*) { return OK; }
};
constexpr char MANIFEST_TMP[] = "manifest", MANIFEST_URL[] = "url";
struct HyphenationManagerActivity {
  static constexpr size_t MAX_PACKS = 48;
""" + pack + r"""
  std::array<Pack, MAX_PACKS> packs_{};
  size_t packCount_ = 0;
  std::string baseUrl_;
  Renderer renderer;
  bool loadManifest();
};
""" + method + r"""
int main(int argc, char** argv) {
  assert(argc == 3);
  std::ifstream input(argv[1]);
  Storage.fixture.assign(std::istreambuf_iterator<char>(input), {});
  HyphenationManagerActivity manager;
  const bool valid = manager.loadManifest();
  const int expectedCount = std::atoi(argv[2]);
  assert(valid == (expectedCount > 0));
  if (valid) {
    assert(manager.packCount_ == static_cast<size_t>(expectedCount));
    assert(manager.packs_[0].supported);
    assert(manager.packCount_ <= manager.MAX_PACKS);
    if (expectedCount > 1) assert(manager.packs_[1].supported);
  }
  assert(peak <= MAX_JSON_MEMORY);
  assert(used == 0 && blocks.empty());
}
"""


def entry(code):
    return {"code": code, "name": code, "file": f"hyph-{code}.cphyph",
            "size": 100, "crc32": 123, "payloadCrc32": 456}


def manifest(packs, **extra):
    return {"version": 1, "baseUrl": "https://example.com/", "packs": packs, **extra}


unknown = [entry(a + b) for a in "abcd" for b in "abcdefghijklmnopqrstuvwxyz"]
cases = [
    (manifest([entry("en")]), 1),
    (manifest([entry("en"), entry("es"), entry("zz")]), 3),
    (manifest(unknown[:46] + [entry("en"), entry("es")]), 48),
    (manifest(unknown[:50] + [entry("en"), entry("es")]), 48),
    (manifest([entry("en"), entry("es")], padding="x" * 24000), 2),
    (manifest([entry("en")], padding="x" * 78000), 0),
    (manifest([entry("en")], padding="x" * 150000), 0),
    (manifest([entry("en"), {**entry("es"), "name": "x" * 24000}]), 0),
    (manifest([entry("en")] * 200), 0),
    (manifest([entry("zz")]), 0),
    (manifest([]), 0),
    (manifest([entry("en"), entry("en")]), 0),
    (manifest([{**entry("en"), "crc32": "invalid"}]), 0),
    (manifest([{**entry("en"), "file": "../other"}]), 0),
    (manifest([{**entry("en"), "name": "x" * 40}]), 0),
    ({**manifest([entry("en")]), "version": 2}, 0),
    ({**manifest([entry("en")]), "baseUrl": "http://example.com/"}, 0),
]
if len(sys.argv) > 1:
    published = json.loads(Path(sys.argv[1]).read_text())
    cases.append((published, min(48, len(published["packs"]))))

with tempfile.TemporaryDirectory() as directory:
    cpp = Path(directory) / "manifest.cpp"
    executable = Path(directory) / "manifest"
    cpp.write_text(harness)
    subprocess.run([
        "c++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
        # Use the firmware's pool sizing; host pointers still have their native size.
        "-DARDUINOJSON_SLOT_ID_SIZE=2", "-DARDUINOJSON_POOL_CAPACITY=128",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-I", str(ROOT / ".pio/libdeps/default/ArduinoJson/src"),
        str(cpp), "-o", str(executable),
    ], check=True)
    for index, (fixture, expected) in enumerate(cases):
        path = Path(directory) / f"case-{index}.json"
        path.write_text(json.dumps(fixture, separators=(",", ":")))
        subprocess.run([str(executable), str(path), str(expected)], check=True)
print(f"Hyphenation manifest checks passed ({len(cases)} cases, 24 KB JSON allocation limit)")
