#ifdef SIMULATOR

#include "SimulatorHeap.h"
#include "SimulatorHeapViz.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "tlsf.h"

// Caller tracking is a host-only debugging aid. Capture uses glibc's
// backtrace() (libc, always present on Linux/macOS); symbol/line resolution
// uses backward-cpp (libdwarf + libelf) when CROSSPOINT_SIM_USE_BACKWARD is
// defined by the PlatformIO simulator build. The unit-test build also compiles
// this file but does not define that macro, so it neither needs backward.hpp
// nor libdwarf.
#if __has_include(<execinfo.h>)
#define SIM_HEAP_TRACE_AVAILABLE 1
#include <execinfo.h>
#else
#define SIM_HEAP_TRACE_AVAILABLE 0
#endif

#if SIM_HEAP_TRACE_AVAILABLE && defined(CROSSPOINT_SIM_USE_BACKWARD)
#define SIM_HEAP_TRACE_BACKWARD 1
#include <unistd.h>  // isatty/fileno for per-frame color decision

#include "backward.hpp"
#else
#define SIM_HEAP_TRACE_BACKWARD 0
#endif

extern "C" void* __real_malloc(std::size_t size);
extern "C" void* __real_calloc(std::size_t nmemb, std::size_t size);
extern "C" void* __real_realloc(void* ptr, std::size_t size);
extern "C" void __real_free(void* ptr);

namespace {

// Match the current gh_release firmware's remaining DRAM budget reported by
// the ESP-IDF size summary (`pio run -e gh_release`): 153,371 bytes free.
constexpr std::size_t kDefaultArenaBytes = 153371U;
constexpr std::size_t kAlignment = alignof(std::max_align_t);

std::size_t alignUp(const std::size_t value, const std::size_t alignment) {
  if (alignment == 0) return value;
  const std::size_t remainder = value % alignment;
  return remainder == 0 ? value : value + (alignment - remainder);
}

// ---------------------------------------------------------------------------
// Caller tracking (heap profiler)
//
// When CROSSPOINT_SIM_HEAP_TRACE=1 is set in the environment, every arena
// allocation records the call stack that requested it. On an OOM (or on demand)
// the live allocations are aggregated by call site and printed largest-first,
// so the culprit holding the heap is obvious instead of just "free=223168
// max_alloc=30448".
//
// Two invariants keep this from perturbing what it measures:
//   1. All bookkeeping (the side table, backtrace internals, backward-cpp) uses
//      the *real* host heap, never the fixed simulator arena, so the reported
//      numbers are
//      unchanged whether tracing is on or off.
//   2. A thread-local reentrancy guard routes any allocation made *inside* the
//      tracer to the real heap. The arena runs under a non-recursive mutex and
//      backtrace()/symbol resolution allocate, so without this guard we would
//      deadlock or recurse.
// ---------------------------------------------------------------------------

// Depth incremented while a host-only code path should bypass the simulator
// arena. The malloc wrappers and operator new check it and fall back to the
// real heap when it is non-zero.
thread_local int gHostHeapScopeDepth = 0;

struct TracerGuard {
  TracerGuard() { ++gHostHeapScopeDepth; }
  ~TracerGuard() { --gHostHeapScopeDepth; }
  TracerGuard(const TracerGuard&) = delete;
  TracerGuard& operator=(const TracerGuard&) = delete;
};

// STL allocator that bypasses the arena entirely by calling the real heap. Used
// for the side table so tracing never consumes the budget it is measuring.
template <class T>
struct RealAllocator {
  using value_type = T;
  RealAllocator() = default;
  template <class U>
  explicit RealAllocator(const RealAllocator<U>&) noexcept {}
  T* allocate(std::size_t n) {
    void* p = __real_malloc(n * sizeof(T));
    if (!p) throw std::bad_alloc();
    return static_cast<T*>(p);
  }
  void deallocate(T* p, std::size_t) noexcept { __real_free(p); }
  template <class U>
  bool operator==(const RealAllocator<U>&) const noexcept {
    return true;
  }
  template <class U>
  bool operator!=(const RealAllocator<U>&) const noexcept {
    return false;
  }
};

constexpr int kMaxTraceFrames = 32;

struct TraceRecord;
std::uint64_t hashTraceRecord(const TraceRecord& rec);

struct TraceRecord {
  std::size_t requested;
  std::uint64_t sequence;
  std::uint64_t runMillis;
  int frameCount;
  void* frames[kMaxTraceFrames];
};

std::atomic<std::uint64_t> gAllocationTraceSequence{0};
const auto gAllocationTraceStart = std::chrono::steady_clock::now();

std::uint64_t currentRunMillis() {
  const auto now = std::chrono::steady_clock::now();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now - gAllocationTraceStart).count());
}

struct TraceCacheKey {
  int frameCount = 0;
  std::array<void*, kMaxTraceFrames> frames = {};

  bool operator==(const TraceCacheKey& other) const {
    if (frameCount != other.frameCount) return false;
    for (int i = 0; i < frameCount; ++i) {
      if (frames[i] != other.frames[i]) return false;
    }
    return true;
  }
};

struct TraceCacheKeyHash {
  std::size_t operator()(const TraceCacheKey& key) const {
    std::uint64_t hash = 1469598103934665603ULL;
    for (int i = 0; i < key.frameCount; ++i) {
      hash ^= reinterpret_cast<std::uintptr_t>(key.frames[i]);
      hash *= 1099511628211ULL;
    }
    return static_cast<std::size_t>(hash);
  }
};

[[maybe_unused]] TraceCacheKey makeTraceCacheKey(const TraceRecord& trace) {
  TraceCacheKey key;
  key.frameCount = trace.frameCount;
  for (int i = 0; i < trace.frameCount && i < kMaxTraceFrames; ++i) {
    key.frames[static_cast<std::size_t>(i)] = trace.frames[i];
  }
  return key;
}

template <class K, class V>
using RealMap = std::unordered_map<K, V, std::hash<K>, std::equal_to<K>, RealAllocator<std::pair<const K, V>>>;

template <class T>
using RealSet = std::unordered_set<T, std::hash<T>, std::equal_to<T>, RealAllocator<T>>;

class TraceRegistry {
 public:
  void setEnabled(const bool enabled) { enabled_ = enabled; }
  bool enabled() const { return enabled_; }

  void record(void* ptr, const std::size_t size) {
#if SIM_HEAP_TRACE_AVAILABLE
    if (!enabled_ || !ptr) return;
    TraceRecord rec;
    rec.requested = size;
    rec.sequence = gAllocationTraceSequence.fetch_add(1) + 1;
    rec.runMillis = currentRunMillis();
    {
      TracerGuard guard;  // backtrace() may lazily malloc (libgcc unwinder init)
      rec.frameCount = backtrace(rec.frames, kMaxTraceFrames);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    live_[ptr] = rec;
#else
    (void)ptr;
    (void)size;
#endif
  }

  void forget(void* ptr) {
    if (!enabled_) return;
    std::lock_guard<std::mutex> lock(mutex_);
    live_.erase(ptr);
  }

  void updateSize(void* ptr, const std::size_t size) {
    if (!enabled_) return;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = live_.find(ptr);
    if (it != live_.end()) it->second.requested = size;
  }

  void dump(const char* reason);
  void fillSnapshotAllocations(std::vector<struct RawVisualizationAllocation>* allocations) const;

 private:
  bool enabled_ = false;
  mutable std::mutex mutex_;
  RealMap<void*, TraceRecord> live_;
};

TraceRegistry gTraceRegistry;
std::mutex gTraceRenderCacheMutex;
std::unordered_map<TraceCacheKey, std::string, TraceCacheKeyHash> gTraceRenderCache;

struct RawVisualizationAllocation;
struct RawVisualizationSnapshot;

class ArenaAllocator {
 public:
  bool initialize(const std::size_t requestedBytes, const bool logInit) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) return true;

    const std::size_t alignedBytes = alignUp(requestedBytes, kAlignment);
    void* slab = __real_malloc(alignedBytes);
    if (!slab) return false;

    tlsf_t tlsf = tlsf_create_with_pool(slab, alignedBytes, 0);
    if (!tlsf) {
      __real_free(slab);
      errno = ENOMEM;
      return false;
    }

    slab_ = static_cast<std::byte*>(slab);
    slabBytes_ = alignedBytes;
    tlsf_ = tlsf;
    freeBytes_ = alignedBytes - tlsf_size(tlsf_);
    minFreeBytes_ = freeBytes_;
    initialized_ = true;

    if (logInit) {
      std::fprintf(stderr, "[SIM] heap arena initialized: total=%zu bytes\n", slabBytes_);
    }
    return true;
  }

  void shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    shutdownLocked();
  }

  bool isInitialized() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return initialized_;
  }

  std::size_t totalBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return slabBytes_;
  }

  std::size_t freeBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return freeBytesLocked();
  }

  std::size_t minFreeBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return minFreeBytes_;
  }

  std::size_t largestFreeBlockBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return largestFreeBlockBytesLocked();
  }

  std::size_t fragmentationPercent() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return fragmentationPercentLocked();
  }

  bool captureVisualizationSnapshot(SimulatorHeapViz::Snapshot* out) const;
  bool captureRawVisualizationSnapshot(RawVisualizationSnapshot* out) const;

  void dumpFreeList(const char* reason) const {
    std::lock_guard<std::mutex> lock(mutex_);
    dumpFreeListLocked(reason);
  }

  std::size_t currentUsedBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t free = freeBytesLocked();
    return slabBytes_ > free ? slabBytes_ - free : 0;
  }

  std::size_t peakUsedBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return slabBytes_ > minFreeBytes_ ? slabBytes_ - minFreeBytes_ : 0;
  }

  bool ownsPointer(const void* ptr) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || !ptr) return false;
    const auto* bytes = static_cast<const std::byte*>(ptr);
    return bytes >= slab_ && bytes < slab_ + slabBytes_;
  }

  void* allocate(std::size_t size, std::size_t alignment = kAlignment) {
    void* ptr = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ptr = allocateLocked(size, alignment);
    }
    if (ptr) gTraceRegistry.record(ptr, size);
    return ptr;
  }

  void* calloc(std::size_t nmemb, std::size_t size) {
    if (nmemb != 0 && size > std::numeric_limits<std::size_t>::max() / nmemb) {
      errno = ENOMEM;
      return nullptr;
    }

    const std::size_t requested = nmemb * size;
    void* ptr = allocate(requested, kAlignment);
    if (!ptr) return nullptr;
    std::memset(ptr, 0, requested == 0 ? 1 : requested);
    return ptr;
  }

  void* reallocate(void* ptr, std::size_t size, std::size_t alignment = kAlignment) {
    if (!ptr) return allocate(size, alignment);
    if (size == 0) {
      deallocate(ptr);
      return nullptr;
    }

    void* result = nullptr;
    void* forgetPtr = nullptr;
    void* recordPtr = nullptr;
    void* updatePtr = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!initialized_) {
        errno = ENOMEM;
        return nullptr;
      }

      if (livePointers_.find(ptr) == livePointers_.end()) {
        errno = EINVAL;
        return nullptr;
      }

      const std::size_t effectiveAlignment = normalizedAlignment(alignment);
      if (effectiveAlignment == 0) return nullptr;

      if (effectiveAlignment <= kTlsfBaseAlignment || size <= tlsf_block_size(ptr)) {
        const std::size_t previousBlockSize = tlsf_block_size(ptr);
        result = tlsf_realloc(tlsf_, ptr, size);
        if (!result) {
          errno = ENOMEM;
          return nullptr;
        }

        freeBytes_ += previousBlockSize;
        freeBytes_ -= tlsf_block_size(result);
        updateMinFreeLocked();

        if (result != ptr) {
          livePointers_.erase(ptr);
          livePointers_.insert(result);
        }

        if ((reinterpret_cast<std::uintptr_t>(result) & (effectiveAlignment - 1U)) != 0) {
          void* alignedResult = allocateLocked(size, effectiveAlignment);
          if (!alignedResult) return nullptr;
          std::memcpy(alignedResult, result, std::min(size, tlsf_block_size(result)));
          freeLocked(result);
          result = alignedResult;
          forgetPtr = ptr;
          recordPtr = alignedResult;
        } else {
          if (result != ptr) {
            forgetPtr = ptr;
            recordPtr = result;
          } else {
            updatePtr = result;
          }
        }
      } else {
        result = allocateLocked(size, effectiveAlignment);
        if (!result) return nullptr;
        std::memcpy(result, ptr, std::min(size, tlsf_block_size(ptr)));
        freeLocked(ptr);
        forgetPtr = ptr;
        recordPtr = result;
      }
    }
    if (forgetPtr) gTraceRegistry.forget(forgetPtr);
    if (recordPtr) gTraceRegistry.record(recordPtr, size);
    if (updatePtr) gTraceRegistry.updateSize(updatePtr, size);
    return result;
  }

  void deallocate(void* ptr) {
    if (!ptr) return;
    bool freed = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      freed = freeLocked(ptr);
    }
    if (freed) gTraceRegistry.forget(ptr);
  }

 private:
  static constexpr std::size_t kTlsfBaseAlignment = 4;

  static std::size_t normalizedAlignment(const std::size_t alignment) {
    const std::size_t effectiveAlignment = alignment < kAlignment ? kAlignment : alignment;
    if ((effectiveAlignment & (effectiveAlignment - 1U)) != 0) {
      errno = EINVAL;
      return 0;
    }
    return effectiveAlignment;
  }

  void updateMinFreeLocked() {
    const std::size_t currentFree = freeBytes_;
    if (currentFree < minFreeBytes_) minFreeBytes_ = currentFree;
  }

  std::size_t freeBytesLocked() const {
    return freeBytes_;
  }

  std::size_t largestFreeBlockBytesLocked() const {
    if (!tlsf_) return 0;
    struct LargestFreeBlockState {
      std::size_t largest = 0;
    } state;
    tlsf_walk_pool(tlsf_get_pool(tlsf_),
                   [](void*, std::size_t size, int used, void* user) {
                     if (!used) {
                       auto& state = *static_cast<LargestFreeBlockState*>(user);
                       if (size > state.largest) state.largest = size;
                     }
                     return true;
                   },
                   &state);
    return tlsf_fit_size(tlsf_, state.largest);
  }

  std::size_t fragmentationPercentLocked() const {
    const std::size_t free = freeBytesLocked();
    if (free == 0) return 0;
    const std::size_t largest = largestFreeBlockBytesLocked();
    if (largest >= free) return 0;
    return ((free - largest) * 100U) / free;
  }

  void dumpFreeListLocked(const char* reason) const {
    constexpr std::size_t kMaxBlocks = 1024;
    std::array<std::size_t, kMaxBlocks> blocks;
    std::size_t numBlocks = 0;
    std::array<std::size_t, 5> histogram = {0, 0, 0, 0, 0};

    if (tlsf_) {
      struct DumpState {
        const ArenaAllocator* self;
        std::array<std::size_t, kMaxBlocks>* blocks;
        std::size_t* numBlocks;
        std::array<std::size_t, 5>* histogram;
      } state{this, &blocks, &numBlocks, &histogram};

      tlsf_walk_pool(tlsf_get_pool(tlsf_),
                     [](void*, std::size_t size, int used, void* user) {
                       if (used) return true;
                       auto& state = *static_cast<DumpState*>(user);
                       const std::size_t bytes = tlsf_fit_size(state.self->tlsf_, size);
                       if (*state.numBlocks < kMaxBlocks) {
                         (*state.blocks)[*state.numBlocks] = bytes;
                       }
                       ++(*state.numBlocks);
                       if (bytes < 1024) {
                         (*state.histogram)[0]++;
                       } else if (bytes < 4096) {
                         (*state.histogram)[1]++;
                       } else if (bytes < 16384) {
                         (*state.histogram)[2]++;
                       } else if (bytes < 65536) {
                         (*state.histogram)[3]++;
                       } else {
                         (*state.histogram)[4]++;
                       }
                       return true;
                     },
                     &state);
    }

    const std::size_t sortCount = std::min(numBlocks, kMaxBlocks);
    std::sort(blocks.begin(), blocks.begin() + sortCount, std::greater<>());

    std::fprintf(stderr,
                 "[SIM] free list%s%s: blocks=%zu free=%zu max_alloc=%zu fragmentation=%zu%% histogram(<1K=%zu "
                 "1-4K=%zu 4-16K=%zu 16-64K=%zu 64K+=%zu)\n",
                 reason ? " " : "", reason ? reason : "", numBlocks, freeBytes_, largestFreeBlockBytesLocked(),
                 fragmentationPercentLocked(), histogram[0], histogram[1], histogram[2], histogram[3], histogram[4]);

    std::fprintf(stderr, "[SIM] largest free blocks:");
    if (numBlocks == 0) {
      std::fprintf(stderr, " none");
    } else {
      const std::size_t shown = std::min<std::size_t>(12, numBlocks);
      for (std::size_t i = 0; i < shown; ++i) {
        std::fprintf(stderr, "%s%zu", i == 0 ? " " : ", ", blocks[i]);
      }
    }
    std::fprintf(stderr, "\n");
  }

  void* allocateLocked(const std::size_t requestedSize, const std::size_t alignment) {
    if (!initialized_) {
      errno = ENOMEM;
      return nullptr;
    }

    if (requestedSize == 0) {
      errno = ENOMEM;
      return nullptr;
    }

    const std::size_t effectiveAlignment = normalizedAlignment(alignment);
    if (effectiveAlignment == 0) return nullptr;

    void* ptr = effectiveAlignment <= kTlsfBaseAlignment ? tlsf_malloc(tlsf_, requestedSize)
                                                         : tlsf_memalign(tlsf_, effectiveAlignment, requestedSize);
    if (!ptr) {
      errno = ENOMEM;
      return nullptr;
    }

    freeBytes_ -= tlsf_block_size(ptr);
    freeBytes_ -= tlsf_alloc_overhead();
    updateMinFreeLocked();
    livePointers_.insert(ptr);
    return ptr;
  }

  bool freeLocked(void* ptr) {
    const auto it = livePointers_.find(ptr);
    if (it == livePointers_.end()) return false;

    freeBytes_ += tlsf_block_size(ptr);
    freeBytes_ += tlsf_alloc_overhead();
    livePointers_.erase(it);
    tlsf_free(tlsf_, ptr);
    return true;
  }

  void shutdownLocked() {
    if (!initialized_) return;
    livePointers_.clear();
    tlsf_ = nullptr;
    __real_free(slab_);
    slab_ = nullptr;
    slabBytes_ = 0;
    freeBytes_ = 0;
    minFreeBytes_ = 0;
    initialized_ = false;
  }

  mutable std::mutex mutex_;
  std::byte* slab_ = nullptr;
  std::size_t slabBytes_ = 0;
  tlsf_t tlsf_ = nullptr;
  std::size_t freeBytes_ = 0;
  std::size_t minFreeBytes_ = 0;
  RealSet<void*> livePointers_;
  bool initialized_ = false;
};

ArenaAllocator gAllocator;
bool gArenaActive = false;

std::uint64_t hashTraceRecord(const TraceRecord& rec) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (int i = 0; i < rec.frameCount; ++i) {
    hash ^= reinterpret_cast<std::uintptr_t>(rec.frames[i]);
    hash *= 1099511628211ULL;
  }
  return hash;
}

#if SIM_HEAP_TRACE_AVAILABLE
// True for allocator-internal frames we hide so the first printed frame is the
// actual caller, not operator new / the arena plumbing.
bool isAllocatorFrame(const char* text) {
  if (!text) return false;
  return std::strstr(text, "SimulatorHeap") || std::strstr(text, "operator new") ||
         std::strstr(text, "__wrap_malloc") || std::strstr(text, "__wrap_calloc") ||
         std::strstr(text, "__wrap_realloc") || std::strstr(text, "TraceRegistry") ||
         std::strstr(text, "ArenaAllocator");
}

// System headers (STL internals, libstdc++) resolve to /usr/include and are
// noise in a heap trace: the snippet is unhelpful and often the source isn't
// even installed. We keep the frame but suppress its source context.
[[maybe_unused]] bool isSystemFile(const std::string& file) { return file.find("/usr/include") != std::string::npos; }

#if SIM_HEAP_TRACE_BACKWARD
// Resolve a stored allocation stack with backward-cpp and render it through
// backward's Printer (function + file:line + source snippet), skipping the
// leading allocator frames. Runs under the caller's TracerGuard.
std::string formatSiteStackBackward(const TraceRecord& rec, const backward::ColorMode::type colorMode) {
  static backward::TraceResolver resolver;

  std::vector<backward::ResolvedTrace> resolved;
  resolved.reserve(static_cast<std::size_t>(rec.frameCount));
  bool reachedCaller = false;
  std::size_t outIdx = 0;
  for (int f = 0; f < rec.frameCount; f++) {
    backward::ResolvedTrace rt = resolver.resolve(backward::Trace(rec.frames[f], 0));
    if (!reachedCaller) {
      const char* fn = !rt.source.function.empty() ? rt.source.function.c_str() : rt.object_function.c_str();
      const char* file = rt.source.filename.c_str();
      if (isAllocatorFrame(fn) || isAllocatorFrame(file)) continue;
      reachedCaller = true;
    }
    rt.idx = outIdx++;  // renumber so backward prints 0..N from the real caller
    resolved.push_back(std::move(rt));
  }

  backward::Printer printer;
  printer.address = true;  // show the raw address too
  printer.object = false;  // skip the object/section noise
  printer.color_mode = colorMode;

  std::ostringstream rendered;
  // Print frame-by-frame so the source snippet can be toggled per frame: keep it
  // for application code, drop it for /usr/include STL frames (no useful context,
  // source often absent). backward's Printer has no header toggle and re-emits
  // "Stack trace (most recent call last):" on every print() call, so render each
  // frame into a buffer and strip that first header line before writing it out.
  for (auto it = resolved.begin(); it != resolved.end(); ++it) {
    printer.snippet = !isSystemFile(it->source.filename);
    auto next = it;
    ++next;

    std::ostringstream oss;
    printer.print(it, next, oss);
    const std::string frame = oss.str();
    const std::size_t bodyStart = frame.find('\n');  // skip backward's repeated header line
    rendered << (bodyStart == std::string::npos ? frame : frame.substr(bodyStart + 1));
  }
  return rendered.str();
}

void printSiteStack(const TraceRecord& rec) {
  // We buffer each frame below, which defeats color_mode::automatic's tty probe
  // on the buffer, so decide color once from the real stderr instead.
  const auto colorMode = isatty(fileno(stderr)) ? backward::ColorMode::always : backward::ColorMode::never;
  const std::string rendered = formatSiteStackBackward(rec, colorMode);
  std::fputs(rendered.c_str(), stderr);
}
#else
// No DWARF backend compiled in: degrade to libc symbol names (no file:line, no
// snippet), but still in-process (no addr2line subprocess).
void printSiteStack(const TraceRecord& rec) {
  char** symbols = backtrace_symbols(rec.frames, rec.frameCount);
  bool reachedCaller = false;
  for (int f = 0; f < rec.frameCount; f++) {
    const char* text = symbols ? symbols[f] : nullptr;
    if (!reachedCaller) {
      if (isAllocatorFrame(text)) continue;
      reachedCaller = true;
    }
    std::fprintf(stderr, "[SIM]       %s\n", text ? text : "??");
  }
  free(symbols);  // backtrace_symbols uses the real heap (not arena-owned)
}
#endif  // SIM_HEAP_TRACE_BACKWARD

// Capture and print the call stack of the allocation that just failed. Unlike
// the live-allocation dump (which needs tracing enabled), this is always useful
// on OOM, so it runs regardless of CROSSPOINT_SIM_HEAP_TRACE. The TracerGuard
// routes printSiteStack's own allocations (vector, backward-cpp) to the real
// heap, since the arena is full at this point.
void printCurrentStack() {
  TracerGuard guard;
  TraceRecord rec;
  rec.requested = 0;
  rec.frameCount = backtrace(rec.frames, kMaxTraceFrames);
  printSiteStack(rec);
}

TraceRecord captureCurrentTrace() {
  TraceRecord rec{};
  TracerGuard guard;
  rec.frameCount = backtrace(rec.frames, kMaxTraceFrames);
  return rec;
}
#endif  // SIM_HEAP_TRACE_AVAILABLE

std::string formatContextTraceForVisualization(const TraceRecord* trace) {
  if (!trace) return "stack unavailable";
#if SIM_HEAP_TRACE_AVAILABLE
#if SIM_HEAP_TRACE_BACKWARD
  const TraceCacheKey cacheKey = makeTraceCacheKey(*trace);
  {
    std::lock_guard<std::mutex> lock(gTraceRenderCacheMutex);
    const auto it = gTraceRenderCache.find(cacheKey);
    if (it != gTraceRenderCache.end()) return it->second;
  }
  const std::string rendered = formatSiteStackBackward(*trace, backward::ColorMode::never);
  {
    std::lock_guard<std::mutex> lock(gTraceRenderCacheMutex);
    gTraceRenderCache.emplace(cacheKey, rendered);
  }
  return rendered;
#else
  char** symbols = backtrace_symbols(trace->frames, trace->frameCount);
  bool reachedCaller = false;
  std::ostringstream body;
  for (int i = 0; i < trace->frameCount; ++i) {
    const char* text = symbols ? symbols[i] : "??";
    if (!reachedCaller) {
      if (isAllocatorFrame(text)) continue;
      reachedCaller = true;
    }
    body << text << "\n";
  }
  free(symbols);
  return body.str();
#endif
#else
  return "stack unavailable";
#endif
}

std::string formatAllocationDetailHeader(const TraceRecord* trace, const std::uintptr_t pointerValue,
                                         const std::size_t offset, const std::size_t requestedBytes,
                                         const std::size_t payloadBytes, const std::size_t headerBytes) {
  std::ostringstream oss;
  oss << "ptr=0x" << std::hex << pointerValue << std::dec << "\n";
  oss << "offset=" << offset << " bytes\n";
  oss << "requested=" << requestedBytes << " bytes\n";
  oss << "arena_payload=" << payloadBytes << " bytes\n";
  oss << "arena_total=" << (payloadBytes + headerBytes) << " bytes";
  if (trace) {
    oss << "\nalloc_seq=" << trace->sequence;
    oss << "\nalloc_run_ms=" << trace->runMillis;
  }
  return oss.str();
}

struct RawVisualizationAllocation {
  std::size_t offset = 0;
  std::size_t payloadBytes = 0;
  std::size_t headerBytes = 0;
  std::size_t requestedBytes = 0;
  std::uintptr_t pointerValue = 0;
  std::uint64_t siteHash = 0;
  bool hasTrace = false;
  TraceRecord trace = {};
};

struct RawVisualizationSnapshot {
  std::size_t arenaBytes = 0;
  std::size_t controlBytes = 0;
  std::size_t sentinelBytes = 0;
  std::size_t freeBytes = 0;
  std::size_t currentUsedBytes = 0;
  std::size_t peakUsedBytes = 0;
  std::size_t minFreeBytes = 0;
  std::size_t largestFreeBlockBytes = 0;
  std::size_t fragmentationPercent = 0;
  std::vector<RawVisualizationAllocation> allocations;
};

void TraceRegistry::fillSnapshotAllocations(std::vector<RawVisualizationAllocation>* allocations) const {
  if (!allocations) return;
  std::lock_guard<std::mutex> lock(mutex_);
  for (RawVisualizationAllocation& allocation : *allocations) {
    const auto it = live_.find(reinterpret_cast<void*>(allocation.pointerValue));
    if (it == live_.end()) continue;
    allocation.hasTrace = true;
    allocation.trace = it->second;
    allocation.requestedBytes = allocation.trace.requested;
    allocation.siteHash = hashTraceRecord(allocation.trace);
  }
}

std::mutex gPendingVisualizationMutex;
bool gPendingPeakVisualizationValid = false;
RawVisualizationSnapshot gPendingPeakVisualization;
const char* gPendingPeakVisualizationReason = nullptr;
TraceRecord gPendingPeakVisualizationTrace{};
bool gPendingPeakVisualizationHasTrace = false;

bool materializeVisualizationSnapshot(const RawVisualizationSnapshot& rawSnapshot, SimulatorHeapViz::Snapshot* out) {
  if (!out) return false;
  out->arenaBytes = rawSnapshot.arenaBytes;
  out->controlBytes = rawSnapshot.controlBytes;
  out->sentinelBytes = rawSnapshot.sentinelBytes;
  out->freeBytes = rawSnapshot.freeBytes;
  out->peakUsedBytes = rawSnapshot.peakUsedBytes;
  out->minFreeBytes = rawSnapshot.minFreeBytes;
  out->largestFreeBlockBytes = rawSnapshot.largestFreeBlockBytes;
  out->fragmentationPercent = rawSnapshot.fragmentationPercent;
  out->allocations.clear();
  out->allocations.reserve(rawSnapshot.allocations.size());

  for (std::size_t i = 0; i < rawSnapshot.allocations.size(); ++i) {
    const RawVisualizationAllocation& raw = rawSnapshot.allocations[i];
    SimulatorHeapViz::AllocationSnapshot allocation;
    allocation.offset = raw.offset;
    allocation.payloadBytes = raw.payloadBytes;
    allocation.headerBytes = raw.headerBytes;
    allocation.requestedBytes = raw.requestedBytes;
    allocation.pointerValue = raw.pointerValue;
    allocation.siteHash = raw.siteHash;
    allocation.titleText =
        "alloc " + std::to_string(i) + " (" + std::to_string(raw.requestedBytes) + " requested, " +
        std::to_string(raw.payloadBytes + raw.headerBytes) + " arena total)";
    allocation.detailHeaderText =
        formatAllocationDetailHeader(raw.hasTrace ? &raw.trace : nullptr, raw.pointerValue, raw.offset,
                                     raw.requestedBytes, raw.payloadBytes, raw.headerBytes);
    allocation.stackText = raw.hasTrace ? formatContextTraceForVisualization(&raw.trace) : "stack unavailable";
    out->allocations.push_back(std::move(allocation));
  }
  std::sort(out->allocations.begin(), out->allocations.end(),
            [](const SimulatorHeapViz::AllocationSnapshot& a, const SimulatorHeapViz::AllocationSnapshot& b) {
              return a.offset < b.offset;
            });
  return true;
}

bool ArenaAllocator::captureRawVisualizationSnapshot(RawVisualizationSnapshot* out) const {
  if (!out) return false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || !tlsf_) return false;

    out->arenaBytes = slabBytes_;
    out->controlBytes = tlsf_size(tlsf_);
    out->sentinelBytes =
        tlsf_pool_overhead() > tlsf_alloc_overhead() ? tlsf_pool_overhead() - tlsf_alloc_overhead() : 0;
    out->freeBytes = freeBytes_;
    out->currentUsedBytes = slabBytes_ > freeBytes_ ? slabBytes_ - freeBytes_ : 0;
    out->peakUsedBytes = slabBytes_ > minFreeBytes_ ? slabBytes_ - minFreeBytes_ : 0;
    out->minFreeBytes = minFreeBytes_;
    out->largestFreeBlockBytes = largestFreeBlockBytesLocked();
    out->fragmentationPercent = fragmentationPercentLocked();
    out->allocations.clear();
    out->allocations.reserve(livePointers_.size());

    for (void* ptr : livePointers_) {
      RawVisualizationAllocation allocation;
      allocation.offset = static_cast<std::size_t>(static_cast<std::byte*>(ptr) - slab_);
      allocation.payloadBytes = tlsf_block_size(ptr);
      allocation.headerBytes = tlsf_alloc_overhead();
      allocation.requestedBytes = allocation.payloadBytes;
      allocation.pointerValue = reinterpret_cast<std::uintptr_t>(ptr);
      allocation.siteHash = allocation.pointerValue;
      out->allocations.push_back(allocation);
    }
  }
  gTraceRegistry.fillSnapshotAllocations(&out->allocations);
  return true;
}

bool ArenaAllocator::captureVisualizationSnapshot(SimulatorHeapViz::Snapshot* out) const {
  if (!out) return false;
  RawVisualizationSnapshot rawSnapshot;
  if (!captureRawVisualizationSnapshot(&rawSnapshot)) return false;
  return materializeVisualizationSnapshot(rawSnapshot, out);
}

void TraceRegistry::dump(const char* reason) {
#if SIM_HEAP_TRACE_AVAILABLE
  if (!enabled_) return;

  // Everything below allocates (aggregation maps, strings, backward-cpp). The
  // arena is full at OOM time, so route all of it to the real heap.
  TracerGuard guard;

  struct Site {
    std::size_t bytes = 0;
    std::size_t count = 0;
    TraceRecord sample = {};
  };

  std::vector<TraceRecord> liveRecords;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    liveRecords.reserve(live_.size());
    for (const auto& [ptr, rec] : live_) {
      (void)ptr;
      liveRecords.push_back(rec);
    }
  }

  std::unordered_map<std::uint64_t, Site> sites;
  std::size_t liveBytes = 0;
  std::size_t liveCount = 0;
  for (const TraceRecord& rec : liveRecords) {
    const std::uint64_t hash = hashTraceRecord(rec);
    Site& site = sites[hash];
    site.bytes += rec.requested;
    site.count += 1;
    site.sample = rec;
    liveBytes += rec.requested;
    liveCount += 1;
  }

  std::vector<const Site*> ranked;
  ranked.reserve(sites.size());
  for (const auto& [hash, site] : sites) ranked.push_back(&site);
  std::sort(ranked.begin(), ranked.end(), [](const Site* a, const Site* b) { return a->bytes > b->bytes; });

  std::fprintf(stderr, "\n[SIM] ===== heap allocation trace: %s =====\n", reason);
  std::fprintf(stderr, "[SIM] %zu live arena allocations totalling %zu bytes across %zu call sites\n", liveCount,
               liveBytes, ranked.size());

  constexpr std::size_t kMaxSites = 15;
  const std::size_t shownSites = std::min(kMaxSites, ranked.size());
  for (std::size_t i = 0; i < shownSites; i++) {
    const Site& site = *ranked[i];
    const std::size_t avg = site.count ? site.bytes / site.count : 0;
    std::fprintf(stderr, "[SIM] --- #%zu: %zu bytes (%zu allocations, avg %zu bytes) ---\n", i + 1, site.bytes,
                 site.count, avg);
    printSiteStack(site.sample);
  }
  std::fprintf(stderr, "[SIM] ===== end heap allocation trace =====\n\n");
#else
  (void)reason;
#endif
}

void dumpVisualizationSnapshot(const char* reason) {
  if (!SimulatorHeapViz::enabled()) return;
  TracerGuard guard;
  SimulatorHeapViz::Snapshot snapshot;
  if (!gAllocator.captureVisualizationSnapshot(&snapshot)) return;
  if (reason && std::strcmp(reason, "benchmark_tick") != 0) {
#if SIM_HEAP_TRACE_AVAILABLE
    const TraceRecord trace = captureCurrentTrace();
    snapshot.contextTitle = reason;
    snapshot.contextText = formatContextTraceForVisualization(&trace);
#else
    snapshot.contextTitle = reason;
    snapshot.contextText = "stack unavailable";
#endif
  }
  SimulatorHeapViz::writeSnapshot(snapshot, reason ? reason : "manual");
}

const char* thresholdReason(const unsigned flags) {
  if ((flags & SimulatorHeapViz::kPeakUsedIncreased) != 0 &&
      (flags & SimulatorHeapViz::kLargestFreeBlockDecreased) != 0) {
    return "peak_used_and_min_largest_free_block";
  }
  if ((flags & SimulatorHeapViz::kPeakUsedIncreased) != 0) return "peak_used";
  if ((flags & SimulatorHeapViz::kLargestFreeBlockDecreased) != 0) return "min_largest_free_block";
  return "manual";
}

void storePendingPeakVisualization(const unsigned flags) {
  RawVisualizationSnapshot snapshot;
  if (!gAllocator.captureRawVisualizationSnapshot(&snapshot)) return;
  std::lock_guard<std::mutex> lock(gPendingVisualizationMutex);
  gPendingPeakVisualization = std::move(snapshot);
  gPendingPeakVisualizationReason = thresholdReason(flags);
#if SIM_HEAP_TRACE_AVAILABLE
  gPendingPeakVisualizationTrace = captureCurrentTrace();
  gPendingPeakVisualizationHasTrace = true;
#else
  gPendingPeakVisualizationHasTrace = false;
#endif
  gPendingPeakVisualizationValid = true;
}

void clearPendingPeakVisualization() {
  std::lock_guard<std::mutex> lock(gPendingVisualizationMutex);
  gPendingPeakVisualizationValid = false;
  gPendingPeakVisualizationReason = nullptr;
  gPendingPeakVisualizationHasTrace = false;
  gPendingPeakVisualizationTrace = TraceRecord{};
  gPendingPeakVisualization = RawVisualizationSnapshot{};
}

void flushPendingPeakVisualizationIfUsageDropped() {
  RawVisualizationSnapshot snapshot;
  const char* reason = nullptr;
  TraceRecord trace{};
  bool hasTrace = false;
  {
    std::lock_guard<std::mutex> lock(gPendingVisualizationMutex);
    if (!gPendingPeakVisualizationValid) return;
    const std::size_t currentUsedBytes = gAllocator.currentUsedBytes();
    if (currentUsedBytes >= gPendingPeakVisualization.currentUsedBytes) return;
    snapshot = gPendingPeakVisualization;
    reason = gPendingPeakVisualizationReason;
    trace = gPendingPeakVisualizationTrace;
    hasTrace = gPendingPeakVisualizationHasTrace;
    gPendingPeakVisualizationValid = false;
    gPendingPeakVisualizationReason = nullptr;
    gPendingPeakVisualizationHasTrace = false;
    gPendingPeakVisualizationTrace = TraceRecord{};
  }

  SimulatorHeapViz::Snapshot materialized;
  if (!materializeVisualizationSnapshot(snapshot, &materialized)) return;
  materialized.contextTitle = reason ? reason : "peak_used";
  materialized.contextText = hasTrace ? formatContextTraceForVisualization(&trace) : "stack unavailable";
  SimulatorHeapViz::writeSnapshot(materialized, reason ? reason : "peak_used");
}

void maybeDumpVisualizationForThreshold() {
  TracerGuard guard;
  const unsigned flags =
      SimulatorHeapViz::updateThresholdState(gAllocator.peakUsedBytes(), gAllocator.largestFreeBlockBytes());
  if (flags == SimulatorHeapViz::kNoThresholdChange) {
    flushPendingPeakVisualizationIfUsageDropped();
    return;
  }

  if ((flags & SimulatorHeapViz::kPeakUsedIncreased) != 0) {
    storePendingPeakVisualization(flags);
  } else if ((flags & SimulatorHeapViz::kLargestFreeBlockDecreased) != 0) {
    dumpVisualizationSnapshot(thresholdReason(flags));
  }

  flushPendingPeakVisualizationIfUsageDropped();
}

[[maybe_unused]] void logHeapFailure(const char* operation, const std::size_t requestedSize,
                                     const std::size_t alignment, const int errorCode) {
  // Lead with the stack of the failing allocation: the culprit's call site is
  // the first thing a reader wants, before the heap-wide stats and the live
  // allocation dump.
#if SIM_HEAP_TRACE_AVAILABLE
  std::fprintf(stderr, "\n[SIM] heap %s failed for %zu bytes; failing allocation stack:\n", operation, requestedSize);
  printCurrentStack();
#endif
  char failureReason[128];
  std::snprintf(failureReason, sizeof(failureReason), "%s failure (%zu bytes requested)", operation, requestedSize);
  std::fprintf(stderr,
               "[SIM] heap %s failed: requested=%zu alignment=%zu free=%zu total=%zu min_free=%zu peak_used=%zu max_alloc=%zu "
               "frag=%zu%% used=%zu errno=%d\n",
               operation, requestedSize, alignment, gAllocator.freeBytes(), gAllocator.totalBytes(),
               gAllocator.minFreeBytes(), gAllocator.peakUsedBytes(), gAllocator.largestFreeBlockBytes(), gAllocator.fragmentationPercent(),
               gAllocator.currentUsedBytes(), errorCode);
  // Route allocations made by OOM diagnostics to the host heap; otherwise the
  // free-list dump can recurse back into the wrapped allocator while the arena
  // mutex is held and wedge before printing the rest of the report.
  TracerGuard guard;
  if (SimulatorHeapViz::enabled()) {
    SimulatorHeapViz::Snapshot snapshot;
    if (gAllocator.captureVisualizationSnapshot(&snapshot)) {
#if SIM_HEAP_TRACE_AVAILABLE
      const TraceRecord trace = captureCurrentTrace();
      snapshot.contextTitle = failureReason;
      snapshot.contextText = formatContextTraceForVisualization(&trace);
#else
      snapshot.contextTitle = failureReason;
      snapshot.contextText = "stack unavailable";
#endif
      SimulatorHeapViz::writeSnapshot(snapshot, failureReason);
    }
  }
  gAllocator.dumpFreeList(operation);
  gTraceRegistry.dump(operation);
  std::fflush(stderr);
}

[[maybe_unused]] void* hostAlignedAllocate(const std::size_t size, const std::size_t alignment) {
  void* ptr = nullptr;
  const std::size_t effectiveAlignment = alignment < sizeof(void*) ? sizeof(void*) : alignment;
  if (posix_memalign(&ptr, effectiveAlignment, size) != 0) return nullptr;
  return ptr;
}

[[maybe_unused]] bool arenaOwnsPointer(const void* ptr) {
  return gAllocator.ownsPointer(ptr);
}

std::size_t parseByteEnv(const char* name) {
  const char* raw = std::getenv(name);
  if (!raw || !*raw) return kDefaultArenaBytes;

  char* end = nullptr;
  errno = 0;
  const unsigned long long parsed = std::strtoull(raw, &end, 10);
  if (errno != 0 || !end || *end != '\0') {
    std::fprintf(stderr, "[SIM] invalid %s=%s, using default heap arena size %zu\n", name, raw, kDefaultArenaBytes);
    return kDefaultArenaBytes;
  }
  if (parsed > std::numeric_limits<std::size_t>::max()) {
    std::fprintf(stderr, "[SIM] %s=%s exceeds size_t range, using default heap arena size %zu\n", name, raw,
                 kDefaultArenaBytes);
    return kDefaultArenaBytes;
  }
  return static_cast<std::size_t>(parsed);
}

bool parseBoolEnv(const char* name) {
  const char* raw = std::getenv(name);
  if (!raw || !*raw) return false;
  return std::strcmp(raw, "0") != 0;
}

[[maybe_unused]] void* cppAllocateNoThrow(const std::size_t size) noexcept {
  const std::size_t requestedSize = size == 0 ? 1 : size;

  if (!gArenaActive || gHostHeapScopeDepth > 0) return __real_malloc(requestedSize);

  void* ptr = gAllocator.allocate(requestedSize);
  if (!ptr) logHeapFailure("operator new nothrow", requestedSize, kAlignment, errno);
  else maybeDumpVisualizationForThreshold();
  return ptr;
}

[[maybe_unused]] void* cppAlignedAllocateNoThrow(const std::size_t size,
                                                 const std::size_t requestedAlignment) noexcept {
  const std::size_t requestedSize = size == 0 ? 1 : size;

  if (!gArenaActive || gHostHeapScopeDepth > 0) return hostAlignedAllocate(requestedSize, requestedAlignment);

  void* ptr = gAllocator.allocate(requestedSize, requestedAlignment);
  if (!ptr) logHeapFailure("aligned operator new nothrow", requestedSize, requestedAlignment, errno);
  else maybeDumpVisualizationForThreshold();
  return ptr;
}

void ensureInitialized() {
  if (gAllocator.isInitialized()) return;
  const std::size_t heapBytes = parseByteEnv("CROSSPOINT_SIM_HEAP_BYTES");
  if (!gAllocator.initialize(heapBytes, true)) {
    std::fprintf(stderr, "[SIM] failed to initialize heap arena: requested_total=%zu errno=%d\n", heapBytes, errno);
  }

  const bool trace = parseBoolEnv("CROSSPOINT_SIM_HEAP_TRACE");
  gTraceRegistry.setEnabled(trace);
  if (trace) {
#if SIM_HEAP_TRACE_BACKWARD
    // Force debuginfod off before backward/libdw ever resolves an address. A
    // heap dump must stay local: a network lookup per frame would be unusably
    // slow, and on hosts where it is configured (e.g. DEBUGINFOD_URLS points at
    // a distro server) libdw pulls in libdebuginfod -> libcurl, which here
    // crashes in a vendored-zlib symbol clash (PNGdec's inflateInit2_ shadows
    // the system zlib). Local DWARF (-g) is all we need.
    ::setenv("DEBUGINFOD_URLS", "", 1);
#endif
#if SIM_HEAP_TRACE_AVAILABLE
    std::fprintf(stderr, "[SIM] heap caller tracking enabled (backward=%d)\n", SIM_HEAP_TRACE_BACKWARD);
#else
    std::fprintf(stderr, "[SIM] heap caller tracking requested but unavailable on this host\n");
#endif
  }
  SimulatorHeapViz::configureFromEnv();
}

}  // namespace

namespace SimulatorHeap {

HostHeapScope::HostHeapScope() { ++gHostHeapScopeDepth; }

HostHeapScope::~HostHeapScope() { --gHostHeapScopeDepth; }

void initializeFromEnv() { ensureInitialized(); }

void activateFromEnv() {
  if (gArenaActive) return;
  ensureInitialized();
  gArenaActive = true;
  std::fprintf(stderr, "[SIM] heap arena activated\n");
}

bool isInitialized() { return gAllocator.isInitialized(); }

bool isActive() { return gArenaActive; }

std::size_t totalBytes() { return gAllocator.totalBytes(); }

std::size_t currentUsedBytes() { return gAllocator.currentUsedBytes(); }

std::size_t peakUsedBytes() { return gAllocator.peakUsedBytes(); }

std::size_t freeBytes() { return gAllocator.freeBytes(); }

std::size_t minFreeBytes() { return gAllocator.minFreeBytes(); }

std::size_t largestFreeBlockBytes() { return gAllocator.largestFreeBlockBytes(); }

std::size_t fragmentationPercent() { return gAllocator.fragmentationPercent(); }

bool isTraceEnabled() { return gTraceRegistry.enabled(); }

void dumpLiveAllocations(const char* reason) {
  dumpVisualizationSnapshot(reason ? reason : "manual");
  gTraceRegistry.dump(reason ? reason : "manual");
}

void dumpFreeList(const char* reason) {
  dumpVisualizationSnapshot(reason ? reason : "manual");
  gAllocator.dumpFreeList(reason ? reason : "manual");
}

void dumpVisualization(const char* reason) { dumpVisualizationSnapshot(reason ? reason : "manual"); }

#ifdef SIMULATOR_HEAP_TESTING
bool resetForTests(const std::size_t arenaBytes) {
  gAllocator.shutdown();
  clearPendingPeakVisualization();
  SimulatorHeapViz::configureFromEnv();
  return gAllocator.initialize(arenaBytes, false);
}

void shutdownForTests() {
  gAllocator.shutdown();
  clearPendingPeakVisualization();
}

void* allocateForTests(const std::size_t size) {
  void* ptr = gAllocator.allocate(size);
  if (ptr) maybeDumpVisualizationForThreshold();
  return ptr;
}

void* allocateAlignedForTests(const std::size_t size, const std::size_t alignment) {
  void* ptr = gAllocator.allocate(size, alignment);
  if (ptr) maybeDumpVisualizationForThreshold();
  return ptr;
}

void* callocForTests(const std::size_t nmemb, const std::size_t size) {
  void* ptr = gAllocator.calloc(nmemb, size);
  if (ptr) maybeDumpVisualizationForThreshold();
  return ptr;
}

void* reallocForTests(void* ptr, const std::size_t size) {
  void* result = gAllocator.reallocate(ptr, size);
  if (result || size == 0) maybeDumpVisualizationForThreshold();
  return result;
}

void* reallocAlignedForTests(void* ptr, const std::size_t size, const std::size_t alignment) {
  void* result = gAllocator.reallocate(ptr, size, alignment);
  if (result || size == 0) maybeDumpVisualizationForThreshold();
  return result;
}

void freeForTests(void* ptr) {
  gAllocator.deallocate(ptr);
  maybeDumpVisualizationForThreshold();
}

void* cppNewForTests(const std::size_t size) {
  if (void* ptr = cppAllocateNoThrow(size)) return ptr;
  throw std::bad_alloc();
}

void* cppNewNoThrowForTests(const std::size_t size) noexcept { return cppAllocateNoThrow(size); }

void* cppAlignedNewForTests(const std::size_t size, const std::size_t alignment) {
  if (void* ptr = cppAlignedAllocateNoThrow(size, alignment)) return ptr;
  throw std::bad_alloc();
}

void* cppAlignedNewNoThrowForTests(const std::size_t size, const std::size_t alignment) noexcept {
  return cppAlignedAllocateNoThrow(size, alignment);
}

void dumpVisualizationForTests(const char* reason) { dumpVisualization(reason); }
#endif

}  // namespace SimulatorHeap

#ifndef SIMULATOR_HEAP_TESTING_NO_GLOBAL_OVERRIDES
extern "C" void __wrap_free(void* ptr);

extern "C" void* __wrap_malloc(std::size_t size) {
  // gHostHeapScopeDepth > 0: allocation made from host-only code (the heap
  // tracer or a bypassed library like SDL); route to the real heap so it
  // neither consumes nor deadlocks on the arena.
  if (!gArenaActive || gHostHeapScopeDepth > 0) return __real_malloc(size);
  void* ptr = gAllocator.allocate(size);
  if (!ptr) logHeapFailure("malloc", size, kAlignment, errno);
  else maybeDumpVisualizationForThreshold();
  return ptr;
}

extern "C" void* __wrap_calloc(std::size_t nmemb, std::size_t size) {
  if (!gArenaActive || gHostHeapScopeDepth > 0) return __real_calloc(nmemb, size);
  void* ptr = gAllocator.calloc(nmemb, size);
  if (!ptr) {
    const std::size_t requested = (nmemb != 0 && size > std::numeric_limits<std::size_t>::max() / nmemb) ? 0 : nmemb * size;
    logHeapFailure("calloc", requested, kAlignment, errno);
  } else maybeDumpVisualizationForThreshold();
  return ptr;
}

extern "C" void* __wrap_realloc(void* ptr, std::size_t size) {
  if (!gArenaActive || gHostHeapScopeDepth > 0) return __real_realloc(ptr, size);
  if (ptr && !arenaOwnsPointer(ptr)) return __real_realloc(ptr, size);
  void* newPtr = gAllocator.reallocate(ptr, size);
  if (!newPtr && size != 0) logHeapFailure("realloc", size, kAlignment, errno);
  else maybeDumpVisualizationForThreshold();
  return newPtr;
}

extern "C" void __wrap_free(void* ptr) {
  if (!ptr) return;
  if (!gArenaActive || !arenaOwnsPointer(ptr)) {
    __real_free(ptr);
    return;
  }
  gAllocator.deallocate(ptr);
  maybeDumpVisualizationForThreshold();
}

void* operator new(std::size_t size) {
  if (void* ptr = cppAllocateNoThrow(size)) return ptr;
  throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
  if (void* ptr = cppAllocateNoThrow(size)) return ptr;
  throw std::bad_alloc();
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return cppAllocateNoThrow(size); }

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { return cppAllocateNoThrow(size); }

void* operator new(std::size_t size, std::align_val_t alignment) {
  const std::size_t requestedAlignment = static_cast<std::size_t>(alignment);
  if (void* ptr = cppAlignedAllocateNoThrow(size, requestedAlignment)) return ptr;
  throw std::bad_alloc();
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
  const std::size_t requestedAlignment = static_cast<std::size_t>(alignment);
  if (void* ptr = cppAlignedAllocateNoThrow(size, requestedAlignment)) return ptr;
  throw std::bad_alloc();
}

void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
  const std::size_t requestedAlignment = static_cast<std::size_t>(alignment);
  return cppAlignedAllocateNoThrow(size, requestedAlignment);
}

void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
  const std::size_t requestedAlignment = static_cast<std::size_t>(alignment);
  return cppAlignedAllocateNoThrow(size, requestedAlignment);
}

void operator delete(void* ptr) noexcept { __wrap_free(ptr); }

void operator delete[](void* ptr) noexcept { __wrap_free(ptr); }

void operator delete(void* ptr, std::size_t) noexcept { __wrap_free(ptr); }

void operator delete[](void* ptr, std::size_t) noexcept { __wrap_free(ptr); }

void operator delete(void* ptr, std::align_val_t) noexcept { __wrap_free(ptr); }

void operator delete[](void* ptr, std::align_val_t) noexcept { __wrap_free(ptr); }

void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept { __wrap_free(ptr); }

void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept { __wrap_free(ptr); }
#endif

#endif
