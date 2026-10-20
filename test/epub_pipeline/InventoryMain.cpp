// epub_build_inventory: an exact allocation inventory of section builds.
//
// Usage: epub_build_inventory <book.epub> <cacheDir> <outDir> [--arena=BYTES] [--spines=A,B,..]
//                             [--viewport=WxH] [--load-only]
//
// For every selected spine it measures ONE build (createSectionFile) and writes raw records to
// <outDir>/spine_<N>.txt, for test/epub_pipeline/inventory_report.py to symbolize. With
// --load-only it measures the book's first open instead (Epub::load on an empty cache, the arena
// lent to it as ReaderActivity lends the secondary framebuffer) and writes <outDir>/load.txt:
//
//   SUMMARY  heap base/peak/end of the build, arena capacity/high-water, the arena's use at the
//            heap's peak, the heap's live bytes at the arena's peak
//   PHASE    per build phase (setup, extract+resident, parse+finalize): heap and arena at the
//            phase start and each one's peak inside it
//   HEAP     every heap site the build touched: allocations and bytes inside the build, live
//            bytes at build start, at the build's heap peak and at build end
//   ARENA_AT_ARENA_PEAK / ARENA_AT_HEAP_PEAK
//            every live allocation in the lent arena at those two instants, with its offset,
//            size and the code that made it
//
// The heap side is HeapTrack's per-build window; the arena side is BuildArena's host-only trace
// hooks (BUILD_ARENA_TRACE), kept as a shadow stack outside the arena. Addresses are written
// relative to the executable's load base (dladdr), which is what addr2line takes.
#include <BuildArena.h>
#include <dlfcn.h>
#include <execinfo.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

#include "Epub.h"
#include "HeapTrack.h"
#include "PipelineRunner.h"

namespace {

struct ArenaEntry {
  size_t off;
  size_t bytes;
  uintptr_t frames[4];
};

constexpr int kMaxEntries = 32768;
ArenaEntry g_stack[kMaxEntries];
int g_top = 0;
ArenaEntry g_atArenaPeak[kMaxEntries];
int g_atArenaPeakTop = 0;
ArenaEntry g_atHeapPeak[kMaxEntries];
int g_atHeapPeakTop = 0;
size_t g_arenaCursorAtHeapPeak = 0;
size_t g_arenaPeak = 0;
size_t g_heapAtArenaPeak = 0;
bool g_overflow = false;

const BuildArena* g_tracked = nullptr;  // the lent arena of the build being measured
bool g_active = false;

struct Phase {
  size_t heapStart = 0, heapPeak = 0;
  size_t arenaStart = 0, arenaPeak = 0;
};
constexpr int kMaxPhases = 8;
Phase g_phases[kMaxPhases];
int g_phase = 0;
size_t g_phaseArenaPeak = 0;
int g_otherArenaAllocs = 0;
size_t g_otherArenaBytes = 0;

// Every arena allocation of 4 KB or more over the build, by (size, caller): how often the build
// needs a full 32 KB inflate ring versus the 16 KB first walk stage, the 8 KB grow block, ...
struct LargeAlloc {
  size_t bytes;
  uintptr_t caller;
  int count;
};
constexpr int kMaxLarge = 64;
LargeAlloc g_large[kMaxLarge];
int g_largeCount = 0;

uintptr_t g_base = 0;
uintptr_t rel(const uintptr_t pc) { return pc >= g_base ? pc - g_base : pc; }

void onAlloc(const BuildArena* arena, const size_t off, const size_t bytes, const size_t cursor) {
  if (!g_active) return;
  if (arena != g_tracked) {
    ++g_otherArenaAllocs;
    g_otherArenaBytes += bytes;
    return;
  }
  heapTrackPause();  // backtrace() must not be counted, nor recurse into the profiler
  void* frames[6] = {};
  const int depth = backtrace(frames, 6);
  heapTrackResume();
  if (g_top >= kMaxEntries) {
    g_overflow = true;
    return;
  }
  ArenaEntry& e = g_stack[g_top++];
  e.off = off;
  e.bytes = bytes;
  // frames[0] is this hook; [1] is whatever called BuildArena::alloc (inlined into its caller).
  for (int i = 0; i < 4; ++i) e.frames[i] = (i + 1 < depth) ? reinterpret_cast<uintptr_t>(frames[i + 1]) : 0;
  if (bytes >= 4096) {
    int i = 0;
    for (; i < g_largeCount; ++i) {
      if (g_large[i].bytes == bytes && g_large[i].caller == e.frames[0]) break;
    }
    if (i == g_largeCount && g_largeCount < kMaxLarge) g_large[g_largeCount++] = {bytes, e.frames[0], 0};
    if (i < g_largeCount) g_large[i].count++;
  }
  if (cursor > g_phaseArenaPeak) g_phaseArenaPeak = cursor;
  if (cursor > g_arenaPeak) {
    g_arenaPeak = cursor;
    g_heapAtArenaPeak = heapTrackLive();
    std::memcpy(g_atArenaPeak, g_stack, sizeof(ArenaEntry) * g_top);
    g_atArenaPeakTop = g_top;
  }
}

void onRewind(const BuildArena* arena, const size_t cursor) {
  if (!g_active || arena != g_tracked) return;
  while (g_top > 0 && g_stack[g_top - 1].off >= cursor) --g_top;
}

void onLane(const BuildArena* arena, const size_t cursor) {
  if (!g_active || arena != g_tracked) return;
  if (g_phase < kMaxPhases) {
    g_phases[g_phase].heapPeak = heapTrackPhaseMark();
    g_phases[g_phase].arenaPeak = g_phaseArenaPeak;
  }
  ++g_phase;
  g_phaseArenaPeak = cursor;
  if (g_phase < kMaxPhases) {
    g_phases[g_phase].heapStart = heapTrackLive();
    g_phases[g_phase].arenaStart = cursor;
  }
}

// Inside the malloc hook: no allocation here, only copies into static storage.
void onHeapPeak() {
  if (!g_active) return;
  std::memcpy(g_atHeapPeak, g_stack, sizeof(ArenaEntry) * g_top);
  g_atHeapPeakTop = g_top;
  g_arenaCursorAtHeapPeak = g_top > 0 ? g_stack[g_top - 1].off + g_stack[g_top - 1].bytes : 0;
}

void writeArena(std::FILE* f, const char* tag, const ArenaEntry* entries, const int count) {
  for (int i = 0; i < count; ++i) {
    const ArenaEntry& e = entries[i];
    std::fprintf(f, "%s off=%zu bytes=%zu f0=0x%lx f1=0x%lx f2=0x%lx f3=0x%lx\n", tag, e.off, e.bytes,
                 static_cast<unsigned long>(rel(e.frames[0])), static_cast<unsigned long>(rel(e.frames[1])),
                 static_cast<unsigned long>(rel(e.frames[2])), static_cast<unsigned long>(rel(e.frames[3])));
  }
}

std::set<int> parseSpines(const char* list) {
  std::set<int> out;
  std::stringstream ss(list);
  std::string item;
  while (std::getline(ss, item, ',')) {
    if (!item.empty()) out.insert(std::atoi(item.c_str()));
  }
  return out;
}

// Opens a measurement window: the heap profiler's, and the shadow stack of `lent`'s allocations.
void openWindow(const BuildArena* lent) {
  g_tracked = lent;
  g_top = 0;
  g_atArenaPeakTop = 0;
  g_atHeapPeakTop = 0;
  g_arenaPeak = 0;
  g_heapAtArenaPeak = 0;
  g_arenaCursorAtHeapPeak = 0;
  g_overflow = false;
  g_phase = 0;
  g_phaseArenaPeak = 0;
  g_otherArenaAllocs = 0;
  g_otherArenaBytes = 0;
  g_largeCount = 0;
  for (auto& p : g_phases) p = Phase{};
  heapTrackWindowBegin();
  g_phases[0].heapStart = heapTrackLive();
  g_phases[0].arenaStart = lent ? lent->used() : 0;
  g_active = true;
}

// Closes it and writes its records to `path` (`spine` -1: a book load, not a section build).
void closeWindow(const std::string& path, const int spine, const BuildArena* lent) {
  g_active = false;
  if (g_phase < kMaxPhases) {
    g_phases[g_phase].heapPeak = heapTrackPhaseMark();
    g_phases[g_phase].arenaPeak = g_phaseArenaPeak;
  }
  heapTrackWindowEnd();
  const HeapTrackWindowSummary w = heapTrackWindowSummary();

  std::FILE* f = std::fopen(path.c_str(), "w");
  if (f == nullptr) {
    std::fprintf(stderr, "cannot write %s\n", path.c_str());
    return;
  }
  std::fprintf(f,
               "SUMMARY spine=%d heapBase=%zu heapPeak=%zu heapAtSnap=%zu heapEnd=%zu arenaCap=%zu "
               "arenaHighWater=%zu arenaAtHeapPeak=%zu heapAtArenaPeak=%zu otherArenaAllocs=%d "
               "otherArenaBytes=%zu overflow=%d\n",
               spine, w.base, w.peak, w.atSnap, w.end, lent ? lent->capacity() : 0, g_arenaPeak,
               g_arenaCursorAtHeapPeak, g_heapAtArenaPeak, g_otherArenaAllocs, g_otherArenaBytes, g_overflow ? 1 : 0);
  const char* names[] = {"setup", "extract+resident", "parse+finalize", "p3", "p4", "p5", "p6", "p7"};
  for (int i = 0; i <= g_phase && i < kMaxPhases; ++i) {
    std::fprintf(f, "PHASE %s heapStart=%zu heapPeak=%zu arenaStart=%zu arenaPeak=%zu\n", spine < 0 ? "load" : names[i],
                 g_phases[i].heapStart, g_phases[i].heapPeak, g_phases[i].arenaStart, g_phases[i].arenaPeak);
  }
  static HeapTrackWindowSite sites[4096];
  const int n = heapTrackWindowSites(sites, 4096);
  for (int i = 0; i < n; ++i) {
    const auto& s = sites[i];
    std::fprintf(f, "HEAP pc=0x%lx ctx1=0x%lx ctx2=0x%lx count=%zu bytes=%zu start=%zu peak=%zu end=%zu max=%zu\n",
                 static_cast<unsigned long>(rel(s.pc)), static_cast<unsigned long>(rel(s.ctx1)),
                 static_cast<unsigned long>(rel(s.ctx2)), s.winCount, s.winBytes, s.startLive, s.peakLive, s.endLive,
                 s.maxSize);
  }
  for (int i = 0; i < g_largeCount; ++i) {
    std::fprintf(f, "ARENA_LARGE bytes=%zu count=%d f0=0x%lx\n", g_large[i].bytes, g_large[i].count,
                 static_cast<unsigned long>(rel(g_large[i].caller)));
  }
  writeArena(f, "ARENA_AT_ARENA_PEAK", g_atArenaPeak, g_atArenaPeakTop);
  writeArena(f, "ARENA_AT_HEAP_PEAK", g_atHeapPeak, g_atHeapPeakTop);
  std::fclose(f);
  std::fprintf(stderr, "%s %d: heap base=%zu peak=%zu (+%zu) end=%zu | arena peak=%zu at-heap-peak=%zu -> %s\n",
               spine < 0 ? "load" : "spine", spine, w.base, w.peak, w.peak - w.base, w.end, g_arenaPeak,
               g_arenaCursorAtHeapPeak, path.c_str());
}

}  // namespace

int main(const int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr,
                 "usage: %s <book.epub> <cacheDir> <outDir> [--arena=BYTES] [--spines=A,B] [--viewport=WxH] "
                 "[--load-only]\n",
                 argv[0]);
    return 2;
  }
  const std::string epubPath = argv[1];
  const std::string cacheDir = argv[2];
  const std::string outDir = argv[3];
  pipeline_harness::Profile profile;
  std::set<int> spines;
  bool loadOnly = false;
  for (int i = 4; i < argc; ++i) {
    if (std::strncmp(argv[i], "--arena=", 8) == 0) {
      profile.lentArenaBytes = static_cast<size_t>(std::strtoul(argv[i] + 8, nullptr, 10));
    } else if (std::strncmp(argv[i], "--spines=", 9) == 0) {
      spines = parseSpines(argv[i] + 9);
    } else if (std::strncmp(argv[i], "--viewport=", 11) == 0) {
      unsigned w = 0, h = 0;
      if (std::sscanf(argv[i] + 11, "%ux%u", &w, &h) == 2) {
        profile.viewportWidth = static_cast<uint16_t>(w);
        profile.viewportHeight = static_cast<uint16_t>(h);
      }
    } else if (std::strcmp(argv[i], "--load-only") == 0) {
      loadOnly = true;
    }
  }

  // Attribute each heap allocation to the application frame above the allocator (HeapTrack.cpp).
  setenv("HEAPTRACK_SKIP_ALLOCATORS", "1", 1);
  // ...and keyed by (site, caller), so a shared helper is split per caller.
  setenv("HEAPTRACK_SITE_BY_CALLER", "1", 1);

  Dl_info info{};
  if (dladdr(reinterpret_cast<void*>(&main), &info) != 0 && info.dli_fbase != nullptr) {
    g_base = reinterpret_cast<uintptr_t>(info.dli_fbase);
  }

  auto& hooks = buildArenaTraceHooks();
  hooks.onAlloc = onAlloc;
  hooks.onRewind = onRewind;
  hooks.onLane = onLane;
  heapTrackSetWindowPeakCallback(onHeapPeak);

  if (loadOnly) {
    // The book's first open as ReaderActivity runs it: Epub::load building book.bin and the CSS
    // cache, with --arena standing in for the lent secondary framebuffer. One window, written to
    // <outDir>/load.txt. The region is allocated before tracking starts, as the framebuffer is.
    BuildArena lent(profile.lentArenaBytes);
    heapTrackBegin();
    openWindow(lent.valid() ? &lent : nullptr);
    Epub epub(epubPath, cacheDir);
    const bool ok = epub.load(true, false, lent.valid() ? &lent : nullptr);
    closeWindow(outDir + "/load.txt", -1, lent.valid() ? &lent : nullptr);
    heapTrackEnd();
    return ok ? 0 : 1;
  }

  pipeline_harness::setBuildBracket([&](const int spine, const bool begin, BuildArena* lent) {
    if (!spines.empty() && spines.count(spine) == 0) return;
    if (begin) {
      openWindow(lent);
      return;
    }
    closeWindow(outDir + "/spine_" + std::to_string(spine) + ".txt", spine, lent);
  });

  // The harness's canonical dump goes to a file: its SPINE lines carry the page counts, which is
  // how a host viewport is matched to the device's pagination.
  std::ofstream sink(outDir + "/dump.txt");
  heapTrackBegin();
  const bool ok = pipeline_harness::runAndDump(epubPath, cacheDir, profile, sink, {}, {});
  heapTrackEnd();
  return ok ? 0 : 1;
}
