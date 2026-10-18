#include <gtest/gtest.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "../../lib/Epub/Epub/css/CssParser.h"
#include "../../lib/ZipFile/ZipFile.h"
#include "Arduino.h"
#include "BuildArena.h"
#include "HalStorage.h"

// ---------------------------------------------------------------------------
// Heap instrumentation: malloc/free/calloc/realloc are interposed and every
// allocation gets a hidden header in front of the user pointer. The header
// carries a magic value (so free/realloc can recognize pointers that did NOT
// come from this malloc — e.g. aligned_alloc, posix_memalign, or allocations
// made inside the hooks themselves — and pass them through untouched) and the
// measurement epoch the block was allocated in (so freeing a block from an
// earlier measurement window can never underflow the live-byte counter of the
// current window).
// ---------------------------------------------------------------------------

// ASan interposes malloc itself and calls it while initialising, before thread_local storage
// exists, so a second interposer here segfaults the binary before main() — and ctest's test
// discovery with it. Under ASan the hooks are compiled out and only the footprint bounds skip.
#if defined(__SANITIZE_ADDRESS__)
#define CSS_TEST_HEAP_HOOKS 0
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define CSS_TEST_HEAP_HOOKS 0
#endif
#endif
#ifndef CSS_TEST_HEAP_HOOKS
#define CSS_TEST_HEAP_HOOKS 1
#endif

#if CSS_TEST_HEAP_HOOKS
static std::atomic<size_t> g_liveBytes{0};
static std::atomic<uint32_t> g_epoch{0};  // current measurement window; 0 = not measuring
static std::atomic<bool> g_tracking{false};
static thread_local bool g_inHook = false;

static void trackAlloc(size_t sz) { g_liveBytes.fetch_add(sz); }

// Reaching the real allocator underneath our own malloc/free overrides. Same technique as
// test/epub_pipeline/HeapTrack.cpp: on Windows go straight to the Win32 heap (there is no
// dlsym/RTLD_NEXT, which is why this suite used to be skipped entirely on MSYS/UCRT64 and
// showed up as a permanent EpubCssPerformanceTest_NOT_BUILT failure under its old name);
// elsewhere resolve the next malloc in the chain, with the glibc weak symbols as a fallback.
#if defined(_WIN32)
static void* rawAlloc(size_t bytes) { return HeapAlloc(GetProcessHeap(), 0, bytes); }
static void rawFree(void* p) {
  if (p) HeapFree(GetProcessHeap(), 0, p);
}
static void* rawRealloc(void* p, size_t bytes) {
  if (!p) return rawAlloc(bytes);
  return HeapReAlloc(GetProcessHeap(), 0, p, bytes);
}
#else
static void* rawAlloc(size_t bytes) {
  using malloc_fn_t = void* (*)(size_t);
  static malloc_fn_t realMalloc = nullptr;
  if (!realMalloc) {
    realMalloc = reinterpret_cast<malloc_fn_t>(dlsym(RTLD_NEXT, "malloc"));
    if (!realMalloc) {
      extern void* __libc_malloc(size_t) __attribute__((weak));
      realMalloc = __libc_malloc;
    }
  }
  return realMalloc ? realMalloc(bytes) : nullptr;
}

static void rawFree(void* p) {
  using free_fn_t = void (*)(void*);
  static free_fn_t realFree = nullptr;
  if (!realFree) {
    realFree = reinterpret_cast<free_fn_t>(dlsym(RTLD_NEXT, "free"));
    if (!realFree) {
      extern void __libc_free(void*) __attribute__((weak));
      realFree = __libc_free;
    }
  }
  if (realFree) realFree(p);
}

static void* rawRealloc(void* p, size_t bytes) {
  using realloc_fn_t = void* (*)(void*, size_t);
  static realloc_fn_t realRealloc = nullptr;
  if (!realRealloc) {
    realRealloc = reinterpret_cast<realloc_fn_t>(dlsym(RTLD_NEXT, "realloc"));
  }
  return realRealloc ? realRealloc(p, bytes) : nullptr;
}
#endif

struct AllocHeader {
  size_t size;
  uint32_t magic;
  uint32_t epoch;  // measurement window this block was allocated in; 0 = untracked
};

static constexpr uint32_t kHeaderMagic = 0xC55AFE17u;
static constexpr size_t kAlign = alignof(std::max_align_t);
static constexpr size_t kHeaderSize = (sizeof(AllocHeader) + kAlign - 1) & ~(kAlign - 1);

// Reading 16 bytes in front of a foreign pointer is technically out of bounds,
// but every allocator we can encounter here keeps its own metadata there, so
// the load is safe in practice; the magic check then rejects the pointer.
static AllocHeader* headerOf(void* ptr) {
  return reinterpret_cast<AllocHeader*>(static_cast<char*>(ptr) - kHeaderSize);
}

extern "C" {

void* malloc(size_t size) {
  if (g_inHook) return rawAlloc(size);  // header-less; free() detects it via the magic check
  g_inHook = true;
  void* raw = rawAlloc(kHeaderSize + size);
  g_inHook = false;
  if (!raw) return nullptr;
  auto* header = static_cast<AllocHeader*>(raw);
  header->size = size;
  header->magic = kHeaderMagic;
  header->epoch = g_tracking ? g_epoch.load(std::memory_order_relaxed) : 0;
  if (header->epoch != 0) trackAlloc(size);
  return static_cast<char*>(raw) + kHeaderSize;
}

void free(void* ptr) {
  if (!ptr) return;
  AllocHeader* header = headerOf(ptr);
  if (header->magic != kHeaderMagic) {
    rawFree(ptr);
    return;
  }
  // Only blocks allocated in the *current* window may shrink its counter.
  if (g_tracking && header->epoch == g_epoch.load(std::memory_order_relaxed)) {
    g_liveBytes.fetch_sub(header->size, std::memory_order_relaxed);
  }
  header->magic = 0;  // poison so a double free is routed to rawFree and crashes loudly there
  g_inHook = true;
  rawFree(header);
  g_inHook = false;
}

void* calloc(size_t nmemb, size_t size) {
  if (size != 0 && nmemb > SIZE_MAX / size) return nullptr;
  const size_t total = nmemb * size;
  void* p = malloc(total);
  if (p) memset(p, 0, total);
  return p;
}

void* realloc(void* ptr, size_t size) {
  if (!ptr) return malloc(size);
  AllocHeader* header = headerOf(ptr);
  if (header->magic != kHeaderMagic) {
    return rawRealloc(ptr, size);
  }
  const size_t oldSize = header->size;
  void* np = malloc(size);
  if (!np) return nullptr;
  memcpy(np, ptr, oldSize < size ? oldSize : size);
  free(ptr);
  return np;
}

}  // extern "C"

static void beginMeasurement() {
  g_liveBytes.store(0);
  g_epoch.fetch_add(1);
  g_tracking.store(true);
}

static void endMeasurement() { g_tracking.store(false); }

// Bytes still allocated when fn returns — the retained footprint of whatever fn built.
static size_t measureLiveBytes(const std::function<void()>& fn) {
  beginMeasurement();
  fn();
  endMeasurement();
  return g_liveBytes.load();
}
#endif  // CSS_TEST_HEAP_HOOKS

static std::vector<uint8_t> readZipEntry(const std::string& epubPath, const char* entryName) {
  ZipFile zip(epubPath);
  size_t size = 0;
  uint8_t* raw = zip.readFileToMemory(entryName, &size, false);
  if (!raw) {
    return {};
  }
  std::vector<uint8_t> buffer(raw, raw + size);
  free(raw);
  return buffer;
}

// Temp paths via std::filesystem rather than mkdtemp/mkstemps + hardcoded "/tmp": those are
// POSIX-only and were part of why this suite never built on Windows. A process-unique counter
// is enough here — these tests are single-process and clean up after themselves.
static std::filesystem::path uniqueTempPath(const char* suffix) {
  // Must be unique ACROSS PROCESSES: ctest -j runs each test as its own process, so a
  // per-process counter alone collides and the tests delete each other's files (seen as
  // parallel-only failures that pass when run serially). Process id + counter + clock.
  static std::atomic<unsigned> counter{0};
  const auto stamp = static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count());
#if defined(_WIN32)
  const unsigned pid = static_cast<unsigned>(GetCurrentProcessId());
#else
  const unsigned pid = static_cast<unsigned>(getpid());
#endif
  char name[96];
  std::snprintf(name, sizeof(name), "csscache-%u-%u-%llx%s", pid, static_cast<unsigned>(counter.fetch_add(1)), stamp,
                suffix);
  return std::filesystem::temp_directory_path() / name;
}

static std::string makeTempDir() {
  const std::filesystem::path dir = uniqueTempPath("");
  std::error_code ec;
  if (!std::filesystem::create_directories(dir, ec) || ec) {
    return {};
  }
  return dir.string();
}

static bool writeTempCssFile(const std::vector<uint8_t>& data, std::string& outPath) {
  outPath = uniqueTempPath(".css").string();
  std::ofstream out(outPath, std::ios::binary);
  if (!out) return false;
  out.write(reinterpret_cast<const char*>(data.data()), data.size());
  out.close();
  return out.good();
}

// Windows refuses to delete a file another handle still has open, and several of these tests
// let the parser's FsFile close via RAII only at end of scope — after the cleanup call. On
// POSIX the unlink succeeds regardless. Temp-file cleanup is housekeeping, not an assertion,
// so a failure here must not fail the test: swallow the error (the OS reclaims the temp dir).
static bool removePath(const std::string& path) {
  std::error_code ec;
  std::filesystem::remove_all(path, ec);
  return !ec;
}

// Restores the stubbed ESP free-heap value even when an ASSERT_* returns early,
// so a low-heap simulation can't leak into tests that run later in this binary.
struct FreeHeapGuard {
  uint32_t saved = ESP.getFreeHeap();
  ~FreeHeapGuard() { ESP.setFreeHeap(saved); }
};

// Persists the stylesheet at cssPath as parser's rules cache the way the device does
// (Epub::parseCssFiles): beginCacheCompile / appendCompiledFromStream / endCacheCompile, which
// stages rules through a temp file. saveToCache() writes the same format from the in-RAM rule
// map instead, and firmware never calls it, so a cache written that way proves nothing about
// the one the reader actually loads.
static ::testing::AssertionResult compileCache(CssParser& parser, const std::string& cssPath) {
  if (!parser.beginCacheCompile()) {
    return ::testing::AssertionFailure() << "beginCacheCompile failed";
  }
  bool appended = false;
  {
    FsFile cssFile;
    if (!Storage.openFileForRead("CSS", cssPath.c_str(), cssFile)) {
      parser.abortCacheCompile();
      return ::testing::AssertionFailure() << "cannot open " << cssPath;
    }
    appended = parser.appendCompiledFromStream(cssFile);
  }
  if (!appended) {
    parser.abortCacheCompile();
    return ::testing::AssertionFailure() << "appendCompiledFromStream failed";
  }
  if (!parser.endCacheCompile()) {
    return ::testing::AssertionFailure() << "endCacheCompile failed";
  }
  return ::testing::AssertionSuccess();
}

// Must match the defaults in scripts/generate_large_css_epub.py, which produced
// test/fixtures/test_large_css.epub.
constexpr size_t kFixtureRuleCount = 1500;

TEST(CssParser, SkipsRulesWithoutSupportedDeclarations) {
  const std::vector<uint8_t> cssData = {'.', 'i', 'g', 'n', 'o', 'r', 'e', 'd', ' ', '{', ' ',  'c', 'o', 'l',
                                        'o', 'r', ':', ' ', 'r', 'e', 'd', ';', ' ', '}', '\n', '.', 'k', 'e',
                                        'p', 't', ' ', '{', ' ', 'f', 'o', 'n', 't', '-', 'w',  'e', 'i', 'g',
                                        'h', 't', ':', ' ', 'b', 'o', 'l', 'd', ';', ' ', '}',  '\n'};
  std::string cssPath;
  ASSERT_TRUE(writeTempCssFile(cssData, cssPath));

  CssParser parser("");
  FsFile cssFile;
  ASSERT_TRUE(Storage.openFileForRead("CSS", cssPath.c_str(), cssFile));
  ASSERT_TRUE(parser.loadFromStream(cssFile));
  EXPECT_EQ(parser.ruleCount(), 1u);

  std::error_code rmEc;
  std::filesystem::remove(cssPath, rmEc);  // best-effort; see removePath()
}

// The invisibility properties are the one place `color` is consulted at all. Only the invisible
// value counts as a supported declaration: transparent / opacity 0 / visibility hidden keep a
// rule, while red / 0.5 / visible leave it as unsupported as it was before -- so a monochrome
// panel does not start retaining every book's colour rules. Companion to the test above, which
// is what caught the first version of this defining `color: red` and doubling its rule count.
TEST(CssParser, KeepsOnlyInvisibleValuesOfInvisibilityProperties) {
  const std::string css =
      ".ocr { color: transparent; }\n"
      ".fill { -webkit-text-fill-color: transparent; }\n"
      ".alpha { color: rgba(0, 0, 0, 0); }\n"
      ".gone { opacity: 0; }\n"
      ".hid { visibility: hidden; }\n"
      ".red { color: red; }\n"
      ".hex { color: #000000; }\n"
      ".dim { opacity: 0.5; }\n"
      ".vis { visibility: visible; }\n";
  const std::vector<uint8_t> cssData(css.begin(), css.end());
  std::string cssPath;
  ASSERT_TRUE(writeTempCssFile(cssData, cssPath));

  CssParser parser("");
  FsFile cssFile;
  ASSERT_TRUE(Storage.openFileForRead("CSS", cssPath.c_str(), cssFile));
  ASSERT_TRUE(parser.loadFromStream(cssFile));
  EXPECT_EQ(parser.ruleCount(), 5u);  // .ocr .fill .alpha .gone .hid

  std::error_code rmEc;
  std::filesystem::remove(cssPath, rmEc);  // best-effort; see removePath()
}

TEST(CssParserCache, CompileLoadAndLowHeapLookup) {
  const std::string epubPath = FIXTURE_EPUB;
  const char* cssEntry = "OEBPS/styles/large.css";
  const std::string cacheDir = makeTempDir();
  const std::string cacheFileRoot = cacheDir;

  const std::vector<uint8_t> cssData = readZipEntry(epubPath, cssEntry);
  ASSERT_FALSE(cssData.empty()) << "Failed to read CSS entry from EPUB";

  std::string cssPath;
  ASSERT_TRUE(writeTempCssFile(cssData, cssPath));

  ASSERT_FALSE(cacheDir.empty());
  {
    CssParser compiler(cacheFileRoot);
    ASSERT_TRUE(compileCache(compiler, cssPath));
    EXPECT_EQ(compiler.ruleCount(), kFixtureRuleCount);
  }
  const std::filesystem::path cacheFilePath = std::filesystem::path(cacheDir) / "css_rules.cache";
  EXPECT_TRUE(std::filesystem::exists(cacheFilePath));

  // Load into a fresh parser, as a later open of the book does. endCacheCompile() already
  // loaded the index into the compiling parser, so loading there again short-circuits,
  // allocates nothing, and the footprint bounds below would pass on 0 bytes.
  CssParser parser(cacheFileRoot);
#if CSS_TEST_HEAP_HOOKS
  const size_t loadedCacheLiveBytes = measureLiveBytes([&] { ASSERT_TRUE(parser.loadFromCache()); });
  EXPECT_EQ(parser.ruleCount(), kFixtureRuleCount);
  printf("CACHE_LOAD_LIVE_BYTES=%zu\n", loadedCacheLiveBytes);
  // The retained footprint after a cache load is the selector index: one
  // 8-byte (hash, offset) entry per rule. Assert the order of magnitude so a
  // regression back to in-RAM rule storage (~275 KB for this fixture) fails loudly,
  // and the floor so a load that measured nothing cannot pass.
  EXPECT_GE(loadedCacheLiveBytes, kFixtureRuleCount * 8);
  EXPECT_LE(loadedCacheLiveBytes, kFixtureRuleCount * 16);
#else
  ASSERT_TRUE(parser.loadFromCache());
  EXPECT_EQ(parser.ruleCount(), kFixtureRuleCount);
#endif

  {
    CssStyle style = parser.resolveStyle("p", "rule0_0");
    ASSERT_TRUE(style.hasTextDecoration());
    ASSERT_EQ(style.textDecoration, CssTextDecoration::Underline);
  }

  // Force low-heap mode: should bypass disk lookup and not crash.
  FreeHeapGuard heapGuard;
  ESP.setFreeHeap(10 * 1024);
  parser.clear();
  ASSERT_TRUE(parser.loadFromCache());

  const CssStyle lowHeapStyle = parser.resolveStyle("p", "rule0_0");
  EXPECT_FALSE(lowHeapStyle.hasTextDecoration());
  const auto lowHeapStats = parser.getResolveStats();
  printf("LOW_HEAP_STATS resolveCalls=%u mapHits=%u hotHits=%u diskHits=%u lowHeapDiskBypasses=%u misses=%u\n",
         lowHeapStats.resolveCalls, lowHeapStats.mapHits, lowHeapStats.hotHits, lowHeapStats.diskHits,
         lowHeapStats.lowHeapDiskBypasses, lowHeapStats.misses);

  // resolveStyle("p", "rule0_0") probes three selectors — "p", ".rule0_0", and
  // "p.rule0_0" — and each probe must skip its disk lookup in low-heap mode.
  EXPECT_EQ(lowHeapStats.lowHeapDiskBypasses, 3u);
  EXPECT_EQ(lowHeapStats.diskHits, 0u);

  removePath(cacheDir);
  std::error_code rmEc;
  std::filesystem::remove(cssPath, rmEc);  // best-effort; see removePath()
}

#if CSS_TEST_HEAP_HOOKS
// clear() and clearCaches()' over-10 KB eviction exist to hand the index's heap back. Both used
// vector::clear(), which keeps the capacity: the 8 B/rule index a first open loads stayed
// allocated for the whole reading session, even under builds that resolve from the borrowed
// arena and never touch the heap vector again.
TEST(CssParserCache, ClearAfterCompileReleasesTheIndex) {
  const std::string cacheDir = makeTempDir();
  const std::vector<uint8_t> cssData = readZipEntry(FIXTURE_EPUB, "OEBPS/styles/large.css");
  ASSERT_FALSE(cssData.empty());
  std::string cssPath;
  ASSERT_TRUE(writeTempCssFile(cssData, cssPath));

  // Epub::parseCssFiles' sequence: compile (which ends by loading the index), then clear().
  CssParser parser(cacheDir);
  const size_t retained = measureLiveBytes([&] {
    ASSERT_TRUE(compileCache(parser, cssPath));
    ASSERT_EQ(parser.ruleCount(), kFixtureRuleCount);
    parser.clear();
  });
  EXPECT_LT(retained, kFixtureRuleCount * 8 / 4) << "clear() kept the index allocated";

  removePath(cacheDir);
  std::error_code rmEc;
  std::filesystem::remove(cssPath, rmEc);
}

TEST(CssParserCache, OversizedIndexEvictionReleasesIt) {
  const std::string cacheDir = makeTempDir();
  const std::vector<uint8_t> cssData = readZipEntry(FIXTURE_EPUB, "OEBPS/styles/large.css");
  ASSERT_FALSE(cssData.empty());
  std::string cssPath;
  ASSERT_TRUE(writeTempCssFile(cssData, cssPath));
  {
    CssParser compiler(cacheDir);
    ASSERT_TRUE(compileCache(compiler, cssPath));
  }

  // 1500 rules x 8 B is over clearCaches()' 10 KB retain limit, so a section build's end evicts it.
  CssParser parser(cacheDir);
  const size_t retained = measureLiveBytes([&] {
    ASSERT_TRUE(parser.loadFromCache());
    parser.clearCaches();
  });
  EXPECT_LT(retained, kFixtureRuleCount * 8 / 4) << "the over-10 KB eviction kept the index allocated";
  EXPECT_TRUE(parser.loadFromCache()) << "an evicted index must reload from disk";
  EXPECT_EQ(parser.ruleCount(), kFixtureRuleCount);

  removePath(cacheDir);
  std::error_code rmEc;
  std::filesystem::remove(cssPath, rmEc);
}
#endif  // CSS_TEST_HEAP_HOOKS

// ---------------------------------------------------------------------------
// Arena CSS: a build running in the
// BORROWED secondary framebuffer resolves CSS out of a BuildArena — the whole
// {hash,CssStyle} ruleset resident when it fits (in-RAM, no disk, FreeInkBook-style),
// else an arena-backed offset index (disk payloads) — with the hot cache disabled.
// Both arena layouts must resolve IDENTICALLY to the heap path, and the resident
// layout must keep resolving under moderate heap pressure (it touches no SD).
// ---------------------------------------------------------------------------
namespace {
bool stylesEqual(const CssStyle& a, const CssStyle& b) {
  auto len = [](const CssLength& l) { return std::make_pair(l.value, static_cast<int>(l.unit)); };
  return a.textAlign == b.textAlign && a.fontStyle == b.fontStyle && a.fontWeight == b.fontWeight &&
         a.textDecoration == b.textDecoration && a.display == b.display && a.verticalAlign == b.verticalAlign &&
         a.listStyleNone == b.listStyleNone && a.pageBreakBefore == b.pageBreakBefore &&
         a.pageBreakAfter == b.pageBreakAfter && a.cssFloat == b.cssFloat && a.smallCaps == b.smallCaps &&
         a.lineHeightMultiplier == b.lineHeightMultiplier && a.fontSizeMultiplier == b.fontSizeMultiplier &&
         a.fontSizeRootRelative == b.fontSizeRootRelative && len(a.textIndent) == len(b.textIndent) &&
         len(a.marginTop) == len(b.marginTop) && len(a.marginBottom) == len(b.marginBottom) &&
         len(a.marginLeft) == len(b.marginLeft) && len(a.marginRight) == len(b.marginRight) &&
         len(a.paddingTop) == len(b.paddingTop) && len(a.paddingBottom) == len(b.paddingBottom) &&
         len(a.paddingLeft) == len(b.paddingLeft) && len(a.paddingRight) == len(b.paddingRight) &&
         len(a.imageHeight) == len(b.imageHeight) && len(a.imageWidth) == len(b.imageWidth) &&
         a.fontFamily == b.fontFamily && a.defined.fontFamily == b.defined.fontFamily;
}
}  // namespace

TEST(CssParserArena, ResidentAndIndexMatchHeapResolution) {
  const std::string epubPath = FIXTURE_EPUB;
  const char* cssEntry = "OEBPS/styles/large.css";
  const std::string cacheDir = makeTempDir();
  ASSERT_FALSE(cacheDir.empty());
  const std::vector<uint8_t> cssData = readZipEntry(epubPath, cssEntry);
  ASSERT_FALSE(cssData.empty());
  std::string cssPath;
  ASSERT_TRUE(writeTempCssFile(cssData, cssPath));

  CssParser parser(cacheDir);
  ASSERT_TRUE(compileCache(parser, cssPath));

  // Probe a spread of real rules plus a couple of guaranteed misses.
  std::vector<std::pair<std::string, std::string>> probes;
  for (int i = 0; i < 25; ++i) probes.emplace_back("p", "rule0_" + std::to_string(i));
  probes.emplace_back("p", "");
  probes.emplace_back("div", "no_such_rule_xyz");

  auto resolveAll = [&](CssParser& p) {
    std::vector<CssStyle> out;
    out.reserve(probes.size());
    for (const auto& pr : probes) out.push_back(p.resolveStyle(pr.first, pr.second));
    return out;
  };

  // 1) Heap baseline (no arena).
  parser.clear();
  ASSERT_TRUE(parser.loadFromCache());
  const std::vector<CssStyle> heapStyles = resolveAll(parser);

  // 2) RESIDENT with dedup: the fixture's 1500 rules cycle through only ~40 distinct styles,
  //    so the pooled resident (index + distinct-style pool) is an order of magnitude smaller
  //    than a flat {hash, CssStyle} array (~168 KB) — proving the Calibre-style dedup — while
  //    resolving byte-for-byte identically to the heap path.
  {
    BuildArena arena(kFixtureRuleCount * 128 + 8192);
    ASSERT_TRUE(arena.valid());
    parser.clear();
    parser.setIndexArena(&arena);
    parser.setLeanResolve(true);
    ASSERT_TRUE(parser.loadFromCache());
    // Pooled resident: 1500 * 8 B index + ~40 * ~108 B styles ≈ 16 KB, far below the ~168 KB a
    // flat array would need and below even the 8 B/rule offset index of 12 KB + a full pool.
    EXPECT_LT(arena.used(), kFixtureRuleCount * 32u) << "expected dedup to shrink the resident ruleset";
    const std::vector<CssStyle> residentStyles = resolveAll(parser);
    for (size_t i = 0; i < probes.size(); ++i) {
      EXPECT_TRUE(stylesEqual(heapStyles[i], residentStyles[i]))
          << "resident mismatch for " << probes[i].first << "." << probes[i].second;
    }
    // No hot cache and no disk reads under RESIDENT mode.
    EXPECT_EQ(parser.getResolveStats().lowHeapDiskBypasses, 0u);
    parser.clear();  // drop the arena view before the arena is destroyed
  }

  // 3) RESIDENT in low-heap mode: 16 KB is below even the 24 KB lean floor, so resolveStyle
  //    runs with disk lookups disallowed. The resident path touches no SD and must still
  //    resolve every probe. (This used 30 KB, which clears the lean floor, so low-heap mode
  //    never engaged; lowHeapSkips pins that it does now.)
  {
    FreeHeapGuard heapGuard;
    ESP.setFreeHeap(16 * 1024);
    BuildArena arena(kFixtureRuleCount * 128 + 8192);
    ASSERT_TRUE(arena.valid());
    parser.clear();
    parser.setIndexArena(&arena);
    parser.setLeanResolve(true);
    ASSERT_TRUE(parser.loadFromCache());
    ASSERT_TRUE(parser.isArenaResident());
    const std::vector<CssStyle> residentStyles = resolveAll(parser);
    for (size_t i = 0; i < probes.size(); ++i) {
      EXPECT_TRUE(stylesEqual(heapStyles[i], residentStyles[i])) << "resident-under-pressure mismatch at " << i;
    }
    const auto stats = parser.getResolveStats();
    EXPECT_EQ(stats.lowHeapSkips, probes.size()) << "low-heap mode did not engage for every resolve";
    EXPECT_GT(stats.lowHeapRescuedHits, 0u) << "resident lookups should still hit in low-heap mode";
    EXPECT_EQ(stats.lowHeapDiskBypasses, 0u);
    parser.clear();
  }

  removePath(cacheDir);
  std::error_code rmEc;
  std::filesystem::remove(cssPath, rmEc);  // best-effort; see removePath()
}

// INDEX-only fallback: when the ruleset is genuinely distinct (no dedup win) and too big for
// the arena, loadArenaResident bails and the parser reads payloads from disk via the offset
// index — still resolving identically to the heap path. Uses a stylesheet whose every rule is
// unique so the pooled resident can't shrink it (unlike the Calibre-style fixture above).
TEST(CssParserArena, IndexOnlyFallbackMatchesHeapResolution) {
  // 400 unique styles: resident is a 3.2 KB index plus a 6.8 KB pool (17 B/style, measured);
  // the offset index alone is 3.2 KB.
  constexpr int kDistinct = 400;
  std::string css;
  for (int i = 0; i < kDistinct; ++i) {
    css += ".u" + std::to_string(i) + " { margin-top: " + std::to_string(i + 1) +
           "px; text-indent: " + std::to_string(i + 1) + "px; }\n";
  }
  const std::string cacheDir = makeTempDir();
  ASSERT_FALSE(cacheDir.empty());
  std::string cssPath;
  ASSERT_TRUE(writeTempCssFile(std::vector<uint8_t>(css.begin(), css.end()), cssPath));

  CssParser parser(cacheDir);
  ASSERT_TRUE(compileCache(parser, cssPath));

  std::vector<std::pair<std::string, std::string>> probes;
  for (int i = 0; i < 30; ++i) probes.emplace_back("p", "u" + std::to_string(i * 13));
  probes.emplace_back("p", "no_such_rule");

  auto resolveAll = [&](CssParser& p) {
    std::vector<CssStyle> out;
    for (const auto& pr : probes) out.push_back(p.resolveStyle(pr.first, pr.second));
    return out;
  };

  parser.clear();
  ASSERT_TRUE(parser.loadFromCache());
  const std::vector<CssStyle> heapStyles = resolveAll(parser);

  // Arena fits the 8 B/rule offset index with 256 B to spare, so loadArenaResident falls back
  // to the disk-backed index. Sized from the index, not the measured pool: 400 distinct records
  // cost at least their 400 length bytes, so no improvement in style compression can make the
  // resident layout fit and quietly turn this into a second resident test.
  const size_t indexBytes = static_cast<size_t>(kDistinct) * 8u;
  BuildArena arena(indexBytes + 256);
  ASSERT_TRUE(arena.valid());
  parser.clear();
  parser.setIndexArena(&arena);
  parser.setLeanResolve(true);
  ASSERT_TRUE(parser.loadFromCache());
  EXPECT_FALSE(parser.isArenaResident()) << "expected the offset index, not the distinct-style pool";
  EXPECT_EQ(arena.used(), indexBytes) << "the offset index should be all that is left in the arena";
  const std::vector<CssStyle> indexStyles = resolveAll(parser);
  for (size_t i = 0; i < probes.size(); ++i) {
    EXPECT_TRUE(stylesEqual(heapStyles[i], indexStyles[i])) << "index-only mismatch at " << i;
  }
  parser.clear();

  removePath(cacheDir);
  std::error_code rmEc;
  std::filesystem::remove(cssPath, rmEc);  // best-effort; see removePath()
}

// Reproduction from a real book (download.epub) reported as a visual regression: rich rules with
// percentage margins, shorthands, pt units, and vertical-align:%. Resident resolution of each
// class must equal the heap path exactly (any divergence = the codec corrupting a real style).
TEST(CssParserArena, ResidentMatchesHeapForRealRichStylesheet) {
  const std::string css =
      ".apnf { font-size: 0.58333em; font-weight: bold; line-height: 1.2; text-decoration: none; "
      "vertical-align: 70%; margin: 0 0 0 0.2em; }\n"
      ".auteur { display: block; margin-bottom: 0%; margin-left: 30%; margin-top: 0%; "
      "page-break-after: avoid; text-align: right; text-indent: 0%; padding: 0%; }\n"
      ".bl { display: block; margin-bottom: 0%; margin-top: 0%; padding: 0%; }\n"
      ".border { color: gray; display: block; height: 2px; margin: 0.2em 0; }\n"
      ".calibre { display: block; font-size: 1.125em; height: 100%; line-height: 1.2; width: 100%; "
      "padding: 0% 0; margin: 0 5pt; }\n"
      ".titre { text-align: center; font-size: 1.5em; font-weight: bold; margin: 1em 0; "
      "page-break-before: always; text-transform: uppercase; }\n";
  const std::string cacheDir = makeTempDir();
  ASSERT_FALSE(cacheDir.empty());
  std::string cssPath;
  ASSERT_TRUE(writeTempCssFile(std::vector<uint8_t>(css.begin(), css.end()), cssPath));

  CssParser parser(cacheDir);
  ASSERT_TRUE(compileCache(parser, cssPath));

  const std::vector<std::string> classes = {"apnf", "auteur", "bl", "border", "calibre", "titre"};
  auto resolveAll = [&](CssParser& p) {
    std::vector<CssStyle> out;
    for (const auto& c : classes) out.push_back(p.resolveStyle("p", c));
    return out;
  };

  parser.clear();
  ASSERT_TRUE(parser.loadFromCache());
  const std::vector<CssStyle> heapStyles = resolveAll(parser);

  BuildArena arena(64 * 1024);
  ASSERT_TRUE(arena.valid());
  parser.clear();
  parser.setIndexArena(&arena);
  parser.setLeanResolve(true);
  ASSERT_TRUE(parser.loadFromCache());
  const std::vector<CssStyle> residentStyles = resolveAll(parser);

  for (size_t i = 0; i < classes.size(); ++i) {
    EXPECT_TRUE(stylesEqual(heapStyles[i], residentStyles[i])) << "resident != heap for ." << classes[i];
  }
  EXPECT_GT(parser.getResolveStats().diskHits, 0u) << "these classes should resolve (hit)";
  parser.clear();
  removePath(cacheDir);
  std::error_code rmEc;
  std::filesystem::remove(cssPath, rmEc);  // best-effort; see removePath()
}

// The resident path must actually HIT for element, class, and combined selectors — not just
// agree with the heap path (which both-miss on unmatched probes would trivially satisfy). Asserts
// each probe returns the styled (non-default) value AND equals the heap resolution.
TEST(CssParserArena, ResidentHitsElementClassAndCombinedSelectors) {
  const std::string css =
      "p { margin-top: 5px; }\n"
      ".note { text-align: center; }\n"
      "div.warn { font-weight: bold; }\n";
  const std::string cacheDir = makeTempDir();
  ASSERT_FALSE(cacheDir.empty());
  std::string cssPath;
  ASSERT_TRUE(writeTempCssFile(std::vector<uint8_t>(css.begin(), css.end()), cssPath));

  CssParser parser(cacheDir);
  ASSERT_TRUE(compileCache(parser, cssPath));

  BuildArena arena(64 * 1024);
  ASSERT_TRUE(arena.valid());
  parser.clear();
  parser.setIndexArena(&arena);
  parser.setLeanResolve(true);
  ASSERT_TRUE(parser.loadFromCache());

  // Element selector "p" must hit.
  const CssStyle p = parser.resolveStyle("p", "");
  EXPECT_TRUE(p.hasMarginTop()) << "resident MISS on element selector 'p'";
  // Class selector ".note" must hit.
  const CssStyle note = parser.resolveStyle("span", "note");
  EXPECT_TRUE(note.hasTextAlign()) << "resident MISS on class selector '.note'";
  EXPECT_EQ(note.textAlign, CssTextAlign::Center);
  // Combined "div.warn" must hit.
  const CssStyle warn = parser.resolveStyle("div", "warn");
  EXPECT_TRUE(warn.hasFontWeight()) << "resident MISS on combined selector 'div.warn'";
  EXPECT_EQ(warn.fontWeight, CssFontWeight::Bold);
  // A genuine non-match must still miss.
  const CssStyle none = parser.resolveStyle("h1", "absent");
  EXPECT_FALSE(none.hasMarginTop());
  EXPECT_FALSE(none.hasTextAlign());

  const auto stats = parser.getResolveStats();
  EXPECT_GT(stats.diskHits, 0u) << "resident produced zero hits for matching selectors";
  parser.clear();
  removePath(cacheDir);
  std::error_code rmEc;
  std::filesystem::remove(cssPath, rmEc);  // best-effort; see removePath()
}

// The sparse codec must round-trip EVERY style field, not just the margins/font-size the other
// fixtures exercise. Resolve a rule that sets a diverse spread of properties through the resident
// (compress-on-load, decompress-on-lookup) path and require it byte-identical to the heap path.
TEST(CssParserArena, ResidentPreservesAllStyleFields) {
  const std::string css =
      ".a { text-align: center; font-weight: bold; font-style: italic; text-decoration: underline; "
      "margin: 2em; padding-left: 5px; padding-right: 3px; text-indent: 1.5em; line-height: 1.6; "
      "font-size: 120%; vertical-align: super; float: left; font-variant: small-caps; "
      "list-style: none; page-break-before: always; page-break-after: always; font-family: 'EB Garamond'; }\n"
      ".b { margin-top: 3px; text-align: right; font-size: 1.25rem; font-family: serif; }\n"
      ".c { display: none; }\n"
      ".d { display: inline; }\n";
  const std::string cacheDir = makeTempDir();
  ASSERT_FALSE(cacheDir.empty());
  std::string cssPath;
  ASSERT_TRUE(writeTempCssFile(std::vector<uint8_t>(css.begin(), css.end()), cssPath));

  CssParser parser(cacheDir);
  ASSERT_TRUE(compileCache(parser, cssPath));

  const std::vector<std::pair<std::string, std::string>> probes = {{"p", "a"},    {"p", "b"},    {"div", "c"},
                                                                   {"span", "a"}, {"p", "none"}, {"span", "d"}};
  auto resolveAll = [&](CssParser& p) {
    std::vector<CssStyle> out;
    for (const auto& pr : probes) out.push_back(p.resolveStyle(pr.first, pr.second));
    return out;
  };

  parser.clear();
  ASSERT_TRUE(parser.loadFromCache());
  const std::vector<CssStyle> heapStyles = resolveAll(parser);
  // Both paths agreeing proves nothing if the disk cache already lost the value.
  EXPECT_EQ(static_cast<int>(heapStyles[5].display), static_cast<int>(CssDisplay::Inline))
      << "display:inline did not survive the disk cache";

  BuildArena arena(64 * 1024);
  ASSERT_TRUE(arena.valid());
  parser.clear();
  parser.setIndexArena(&arena);
  parser.setLeanResolve(true);
  ASSERT_TRUE(parser.loadFromCache());
  const std::vector<CssStyle> residentStyles = resolveAll(parser);
  for (size_t i = 0; i < probes.size(); ++i) {
    EXPECT_TRUE(stylesEqual(heapStyles[i], residentStyles[i]))
        << "field round-trip mismatch for " << probes[i].first << "." << probes[i].second;
  }
  parser.clear();
  removePath(cacheDir);
  std::error_code rmEc;
  std::filesystem::remove(cssPath, rmEc);  // best-effort; see removePath()
}

// Regression: font-size from stylesheets must survive the disk-cache round trip.
// Before cache v13, writeCssStylePayload dropped fontSizeMultiplier, so class-based
// font-size (e.g. Alice's mouse-tale .taleN spans) silently vanished on device —
// only inline style="" attributes (which bypass the cache) kept their sizes.
TEST(CssParserCache, FontSizeMultiplierSurvivesDiskCache) {
  const std::string cacheDir = makeTempDir();
  ASSERT_FALSE(cacheDir.empty());

  const std::string css =
      ".tale1 { font-size: 95% }\n"
      ".tale5 { font-size: 55% }\n"
      ".em120 { font-size: 1.2em }\n"
      ".plain { text-align: center }\n";
  std::string cssPath;
  ASSERT_TRUE(writeTempCssFile(std::vector<uint8_t>(css.begin(), css.end()), cssPath));

  CssParser parser(cacheDir);
  ASSERT_TRUE(compileCache(parser, cssPath));

  // Fresh state: resolve purely from the cache file, as the device does.
  parser.clear();
  ASSERT_TRUE(parser.loadFromCache());

  const CssStyle tale1 = parser.resolveStyle("span", "tale1");
  ASSERT_TRUE(tale1.hasFontSizeMultiplier());
  EXPECT_FLOAT_EQ(tale1.fontSizeMultiplier, 0.95f);

  const CssStyle tale5 = parser.resolveStyle("span", "tale5");
  ASSERT_TRUE(tale5.hasFontSizeMultiplier());
  EXPECT_FLOAT_EQ(tale5.fontSizeMultiplier, 0.55f);

  const CssStyle em120 = parser.resolveStyle("span", "em120");
  ASSERT_TRUE(em120.hasFontSizeMultiplier());
  EXPECT_FLOAT_EQ(em120.fontSizeMultiplier, 1.2f);

  // A rule without font-size must not gain one through the new payload bytes.
  const CssStyle plain = parser.resolveStyle("span", "plain");
  EXPECT_FALSE(plain.hasFontSizeMultiplier());

  removePath(cacheDir);
  std::error_code rmEc;
  std::filesystem::remove(cssPath, rmEc);  // best-effort; see removePath()
}

// Regression (cache v14): list-style-type:none and page-break-before/after from
// stylesheets must survive the disk-cache round trip. Same defect class as the
// v13 font-size fix — parsed correctly, then dropped by write/readCssStylePayload.
TEST(CssParserCache, ListStyleAndPageBreaksSurviveDiskCache) {
  const std::string cacheDir = makeTempDir();
  ASSERT_FALSE(cacheDir.empty());

  const std::string css =
      ".plainlist { list-style-type: none }\n"
      ".chapter { page-break-before: always }\n"
      ".part { page-break-after: always }\n"
      ".plain { text-align: center }\n";
  std::string cssPath;
  ASSERT_TRUE(writeTempCssFile(std::vector<uint8_t>(css.begin(), css.end()), cssPath));

  CssParser parser(cacheDir);
  ASSERT_TRUE(compileCache(parser, cssPath));
  parser.clear();
  ASSERT_TRUE(parser.loadFromCache());

  const CssStyle plainlist = parser.resolveStyle("ul", "plainlist");
  EXPECT_TRUE(plainlist.hasListStyleNone());
  EXPECT_TRUE(plainlist.listStyleNone);

  const CssStyle chapter = parser.resolveStyle("div", "chapter");
  EXPECT_TRUE(chapter.hasPageBreakBefore());
  EXPECT_TRUE(chapter.pageBreakBefore);
  EXPECT_FALSE(chapter.pageBreakAfter);

  const CssStyle part = parser.resolveStyle("div", "part");
  EXPECT_TRUE(part.hasPageBreakAfter());
  EXPECT_TRUE(part.pageBreakAfter);
  EXPECT_FALSE(part.pageBreakBefore);

  const CssStyle plain = parser.resolveStyle("p", "plain");
  EXPECT_FALSE(plain.hasListStyleNone());
  EXPECT_FALSE(plain.hasPageBreakBefore());
  EXPECT_FALSE(plain.hasPageBreakAfter());

  removePath(cacheDir);
  std::error_code rmEc;
  std::filesystem::remove(cssPath, rmEc);  // best-effort; see removePath()
}

// A compile that hits MAX_RULES mid-stream persists a cache that holds the first MAX_RULES rules
// and is STAMPED truncated (flags byte bit 1), so the book is parsed once and every later open
// says what is missing (rulesTruncated()). It used to write no cache at all, which re-parsed the
// book -- and hit the same cap -- on every open: the styles past the cap were lost either way,
// only slower and silently (memory audit 2026-09, R4).
TEST(CssParserCache, RuleCapExceededPersistsACacheMarkedTruncated) {
  const std::string cacheDir = makeTempDir();
  ASSERT_FALSE(cacheDir.empty());

  // One more unique selector than MAX_RULES (1500) so the compile pipeline hits the cap
  // mid-stream, matching an Amazon-style per-chapter EPUB with more unique classes than fit.
  std::string css;
  css.reserve(64 * 1024);
  for (size_t i = 0; i < 1600; ++i) {
    css += ".rule" + std::to_string(i) + " { font-weight: bold }\n";
  }
  std::string cssPath;
  ASSERT_TRUE(writeTempCssFile(std::vector<uint8_t>(css.begin(), css.end()), cssPath));

  {
    CssParser parser(cacheDir);
    ASSERT_TRUE(parser.beginCacheCompile());
    {
      FsFile cssFile;
      ASSERT_TRUE(Storage.openFileForRead("CSS", cssPath.c_str(), cssFile));
      parser.appendCompiledFromStream(cssFile);
    }
    EXPECT_TRUE(parser.endCacheCompile());
    EXPECT_TRUE(parser.hasCache());
    EXPECT_TRUE(parser.rulesTruncated());
  }

  // A fresh parser loading that cache knows it is truncated, serves the rules that fit, and
  // resolves nothing for the ones past the cap.
  CssParser loaded(cacheDir);
  ASSERT_TRUE(loaded.hasCache());
  ASSERT_TRUE(loaded.loadFromCache());
  EXPECT_TRUE(loaded.rulesTruncated());
  EXPECT_EQ(loaded.ruleCount(), 1500u);
  EXPECT_TRUE(loaded.resolveStyle("p", "rule0").defined.anySet());
  EXPECT_TRUE(loaded.resolveStyle("p", "rule1499").defined.anySet());
  EXPECT_FALSE(loaded.resolveStyle("p", "rule1599").defined.anySet());

  removePath(cacheDir);
  std::error_code rmEc;
  std::filesystem::remove(cssPath, rmEc);  // best-effort; see removePath()
}

// Background pictures (CssParser::backgroundImageFor). They live outside the style records, in a
// side file written at the end of the compile, so the test that matters is the one that compiles,
// drops everything and asks again -- the reader asks long after the compile, from a fresh parser.
TEST(CssParserBackground, ParsesTheUrlAndTheRepeatOfADeclarationList) {
  const auto bg = CssParser::parseBackground(
      "width: 600px; BACKGROUND-IMAGE: url( \"img/hole.png\" ); background-repeat: no-repeat");
  EXPECT_TRUE(bg.declared);
  EXPECT_EQ("img/hole.png", bg.url);
  EXPECT_TRUE(bg.noRepeat);

  const auto shorthand = CssParser::parseBackground("background: #fff url('a.png') no-repeat center top");
  EXPECT_EQ("a.png", shorthand.url);
  EXPECT_TRUE(shorthand.noRepeat);

  const auto tiled = CssParser::parseBackground("background-image: url(tile.png)");
  EXPECT_EQ("tile.png", tiled.url);
  EXPECT_FALSE(tiled.noRepeat);

  // A colour or `none` replaces the picture; data: and remote URLs are not in the book.
  EXPECT_TRUE(CssParser::parseBackground("background-image: url(a.png); background: white").declared);
  EXPECT_TRUE(CssParser::parseBackground("background-image: url(a.png); background: white").url.empty());
  EXPECT_TRUE(CssParser::parseBackground("background-image: url(data:image/png;base64,AAAA)").url.empty());
  EXPECT_TRUE(CssParser::parseBackground("background-image: url(http://example.com/a.png)").url.empty());
  EXPECT_FALSE(CssParser::parseBackground("margin: 0; color: red").declared);
}

TEST(CssParserBackground, CompiledPicturesSurviveAFreshParser) {
  const std::string css =
      "table.rabbithole { width: 600px; text-align: center;\n"
      "  background-image: url(23934376999044308_p0003-rabbithole.png); background-repeat: no-repeat }\n"
      ".only-picture { background: url(\"../Images/only.png\") no-repeat }\n"  // no style declaration at all
      ".tiled { background-image: url(paper.png) }\n"                          // repeats: a texture
      ".cancelled { background-image: url(x.png); background-repeat: no-repeat }\n"
      ".cancelled { background: none }\n"
      "div#hero { background: url(hero.png) no-repeat }\n";
  const std::vector<uint8_t> cssData(css.begin(), css.end());
  std::string cssPath;
  ASSERT_TRUE(writeTempCssFile(cssData, cssPath));
  const std::string cacheDir = makeTempDir();

  {
    CssParser parser(cacheDir);
    ASSERT_TRUE(parser.beginCacheCompile());
    parser.setStylesheetPath("OEBPS/Styles/main.css");
    FsFile cssFile;
    ASSERT_TRUE(Storage.openFileForRead("CSS", cssPath.c_str(), cssFile));
    ASSERT_TRUE(parser.appendCompiledFromStream(cssFile));
    cssFile.close();
    ASSERT_TRUE(parser.endCacheCompile());
  }

  CssParser parser(cacheDir);
  ASSERT_TRUE(parser.loadFromCache());
  const std::string* hole = parser.backgroundImageFor("table", "rabbithole");
  ASSERT_NE(nullptr, hole);
  EXPECT_EQ("OEBPS/Styles/23934376999044308_p0003-rabbithole.png", *hole);  // the sheet's directory
  const std::string* only = parser.backgroundImageFor("div", "other only-picture");
  ASSERT_NE(nullptr, only);
  EXPECT_EQ("OEBPS/Styles/../Images/only.png", *only);  // the parser normalises it
  EXPECT_EQ(nullptr, parser.backgroundImageFor("div", "tiled"));
  EXPECT_EQ(nullptr, parser.backgroundImageFor("p", "cancelled"));
  EXPECT_EQ(nullptr, parser.backgroundImageFor("table", ""));  // the rule was table.rabbithole
  const std::string* hero = parser.backgroundImageFor("div", "", "HERO");
  ASSERT_NE(nullptr, hero);
  EXPECT_EQ("OEBPS/Styles/hero.png", *hero);

  // clear() ends a build; the next one reads the side file again.
  parser.clear();
  EXPECT_NE(nullptr, parser.backgroundImageFor("table", "rabbithole"));

  removePath(cacheDir);
  std::error_code rmEc;
  std::filesystem::remove(cssPath, rmEc);
}

TEST(CssParserBackground, ABookWithoutPicturesLeavesNoSideFile) {
  const std::string css = "p { margin: 0 }\n.tiled { background-image: url(paper.png) }\n";
  const std::vector<uint8_t> cssData(css.begin(), css.end());
  std::string cssPath;
  ASSERT_TRUE(writeTempCssFile(cssData, cssPath));
  const std::string cacheDir = makeTempDir();
  CssParser parser(cacheDir);
  ASSERT_TRUE(compileCache(parser, cssPath));
  EXPECT_FALSE(std::filesystem::exists(std::filesystem::path(cacheDir) / "css_bg.bin"));
  EXPECT_EQ(nullptr, parser.backgroundImageFor("div", "tiled"));
  removePath(cacheDir);
  std::error_code rmEc;
  std::filesystem::remove(cssPath, rmEc);
}

// @font-face rules and font-family: what a book's embedded fonts are and which elements ask for
// them. Ported from crosspoint-reader PR #3934 (Justin Mitchell): CollectsFontFacesAndFontFamilies,
// reworked for this parser's compile path and side file.
TEST(CssParserFonts, FontFamilyIsTheFirstFamilyHashedAndSurvivesTheCache) {
  const std::string css =
      ".end { font-family: \"KubisHandscript\", serif; text-align: center }\n"
      ".plain { font-family: serif }\n"  // generic: the reader's font, or the built-in serif
      ".spaced { font-family:  'EB   Garamond' , Georgia }\n"
      ".only { font-family: Garamond }\n"  // a rule with nothing else is kept
      ".sans { font-family: 'Gill Sans', Helvetica, sans-serif, serif }\n"
      ".mono { font-family: Courier, monospace, serif }\n"  // the list's first generic has its say
      ".keyword { font-family: inherit }\n";
  std::string cssPath;
  ASSERT_TRUE(writeTempCssFile(std::vector<uint8_t>(css.begin(), css.end()), cssPath));
  const std::string cacheDir = makeTempDir();
  CssParser parser(cacheDir);
  ASSERT_TRUE(compileCache(parser, cssPath));
  ASSERT_TRUE(parser.loadFromCache());

  const CssStyle end = parser.resolveStyle("p", "end");
  ASSERT_TRUE(end.defined.fontFamily);
  EXPECT_EQ(CssParser::fontFamilyHash("kubishandscript") | CssFontFamily::kSerif, end.fontFamily);
  EXPECT_EQ(CssTextAlign::Center, end.textAlign);
  const CssStyle plain = parser.resolveStyle("p", "plain");
  ASSERT_TRUE(plain.defined.fontFamily);
  EXPECT_EQ(CssFontFamily::kSerif, plain.fontFamily);
  EXPECT_EQ(0u, CssFontFamily::identity(plain.fontFamily));
  EXPECT_EQ(CssParser::fontFamilyHash("eb garamond"), parser.resolveStyle("p", "spaced").fontFamily);
  EXPECT_EQ(CssParser::fontFamilyHash("Garamond"), parser.resolveStyle("p", "only").fontFamily);
  EXPECT_EQ(CssParser::fontFamilyHash("gill sans") | CssFontFamily::kSansSerif,
            parser.resolveStyle("p", "sans").fontFamily);
  EXPECT_EQ(CssParser::fontFamilyHash("courier"), parser.resolveStyle("p", "mono").fontFamily);
  EXPECT_EQ(0u, parser.resolveStyle("p", "keyword").fontFamily);
  EXPECT_FALSE(parser.resolveStyle("p", "").defined.fontFamily);
  EXPECT_NE(0u, CssParser::fontFamilyHash(""));  // never 0, which means the reader's font
  EXPECT_EQ(0u, CssParser::fontFamilyHash("Garamond") & CssFontFamily::kGenericMask);  // the generic's bits

  // Inline styles take the same path.
  EXPECT_EQ(CssParser::fontFamilyHash("Garamond"), CssParser::parseInlineStyle("font-family: GARAMOND").fontFamily);

  parser.clear();
  removePath(cacheDir);
  std::error_code rmEc;
  std::filesystem::remove(cssPath, rmEc);
}

TEST(CssParserFonts, FontFacesAreRecordedInASideFile) {
  const std::string css =
      "@font-face { font-family: \"KubisHandscript\"; src: url(../Fonts/kubis.ttf); }\n"
      "@font-face { font-family: 'Garamond'; font-weight: bold; font-style: italic;\n"
      "  src: url('g.woff2') format('woff2'), url(\"g-bi.woff\") format(\"woff\"),\n"
      "       url(\"../Fonts/g-bi.ttf\") format(\"truetype\"); }\n"
      "@font-face { font-family: Web; src: url(w.woff2) format(\"woff2\"); }\n"  // nothing usable
      "@font-face { font-family: Data; src: url(data:font/ttf;base64,AAAA); }\n"
      "@font-face { font-family: Semi; font-weight: 600; src: local('Semi'), url(semi.otf?#x) }\n"
      "@media screen { .x { font-weight: bold } }\n"
      "p { margin: 0 }\n";
  std::string cssPath;
  ASSERT_TRUE(writeTempCssFile(std::vector<uint8_t>(css.begin(), css.end()), cssPath));
  const std::string cacheDir = makeTempDir();
  {
    CssParser parser(cacheDir);
    ASSERT_TRUE(parser.beginCacheCompile());
    parser.setStylesheetPath("OEBPS/Styles/main.css");
    FsFile cssFile;
    ASSERT_TRUE(Storage.openFileForRead("CSS", cssPath.c_str(), cssFile));
    ASSERT_TRUE(parser.appendCompiledFromStream(cssFile));
    cssFile.close();
    ASSERT_TRUE(parser.endCacheCompile());
    // @font-face and @media bodies are not style rules.
    EXPECT_EQ(1u, parser.ruleCount());
    EXPECT_FALSE(parser.resolveStyle("p", "x").hasFontWeight());
  }

  std::string records;
  ASSERT_TRUE(CssParser::loadFontFaces(cacheDir, records));
  std::vector<CssParser::FontFace> faces;
  std::string_view rest(records);
  for (CssParser::FontFace face; CssParser::nextFontFace(rest, face);) faces.push_back(face);
  EXPECT_TRUE(rest.empty());
  ASSERT_EQ(3u, faces.size());
  EXPECT_EQ(CssParser::fontFamilyHash("kubishandscript"), faces[0].family);
  EXPECT_EQ("KubisHandscript", faces[0].name);
  EXPECT_EQ(400, faces[0].weight);
  EXPECT_FALSE(faces[0].italic);
  EXPECT_EQ("OEBPS/Styles/../Fonts/kubis.ttf", faces[0].href);
  EXPECT_EQ("Garamond", faces[1].name);
  EXPECT_EQ(700, faces[1].weight);
  EXPECT_TRUE(faces[1].italic);
  EXPECT_EQ("OEBPS/Styles/../Fonts/g-bi.ttf", faces[1].href);  // TrueType before WOFF, never WOFF2
  EXPECT_EQ(600, faces[2].weight);
  EXPECT_EQ("OEBPS/Styles/semi.otf", faces[2].href);

  // A recompile without faces removes the side file.
  const std::string plain = "p { margin: 0 }\n";
  std::string plainPath;
  ASSERT_TRUE(writeTempCssFile(std::vector<uint8_t>(plain.begin(), plain.end()), plainPath));
  CssParser parser(cacheDir);
  ASSERT_TRUE(compileCache(parser, plainPath));
  EXPECT_FALSE(CssParser::loadFontFaces(cacheDir, records));
  EXPECT_TRUE(records.empty());

  parser.clear();
  removePath(cacheDir);
  std::error_code rmEc;
  std::filesystem::remove(cssPath, rmEc);
  std::filesystem::remove(plainPath, rmEc);
}

// Which src entry a face records: refused kinds (WOFF2, SVG, EOT) by extension or format() first,
// then TrueType over OpenType over anything else, the first of a rank winning; and the at-rule is
// found by its prelude whatever its case or spacing. A damaged record ends the list.
TEST(CssParserFonts, TheSrcEntryIsRankedByWhatTheReaderDraws) {
  const std::string css =
      "@font-face{font-family:A;src:url(a.ttf) format('woff2'), url(a.woff)}\n"
      "@FONT-FACE /* c */ { font-family: B; src: url(b1.otf), url(b2.otf) format('truetype'), url(b3.ttf) }\n"
      "@font-face { font-family: C; src: url(c.eot?#iefix) format('embedded-opentype'), url(c.svg#x),"
      " url(c.woff2), url(c.otf) }\n"
      "@font-face { font-family: D; src: url(d.ttf) format('embedded-opentype'), url(noext) }\n"
      "@import url(other.css);\n"
      "@font-face { font-family: E !important; font-weight: 300; src: url(e.ttf) !important }\n";
  std::string cssPath;
  ASSERT_TRUE(writeTempCssFile(std::vector<uint8_t>(css.begin(), css.end()), cssPath));
  const std::string cacheDir = makeTempDir();
  {
    CssParser parser(cacheDir);
    ASSERT_TRUE(compileCache(parser, cssPath));
    parser.clear();
  }
  std::string records;
  ASSERT_TRUE(CssParser::loadFontFaces(cacheDir, records));
  std::vector<CssParser::FontFace> faces;
  std::string_view rest(records);
  for (CssParser::FontFace face; CssParser::nextFontFace(rest, face);) faces.push_back(face);
  ASSERT_EQ(4u, faces.size());
  EXPECT_EQ("A", faces[0].name);
  EXPECT_EQ("a.woff", faces[0].href);
  EXPECT_EQ("b2.otf", faces[1].href);  // TrueType by its format(), and before b3.ttf
  EXPECT_EQ("c.otf", faces[2].href);
  EXPECT_EQ("E", faces[3].name);
  EXPECT_EQ(300, faces[3].weight);
  EXPECT_EQ("e.ttf", faces[3].href);

  std::string_view damaged(records.data(), records.size() - 1);
  size_t whole = 0;
  for (CssParser::FontFace face; CssParser::nextFontFace(damaged, face);) ++whole;
  EXPECT_EQ(3u, whole);

  removePath(cacheDir);
  std::error_code rmEc;
  std::filesystem::remove(cssPath, rmEc);
}
