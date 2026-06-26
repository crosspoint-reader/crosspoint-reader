#ifdef SIMULATOR

#include "SimulatorHeap.h"

#include <algorithm>
#include <array>
#include <cerrno>
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
#include <vector>

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

constexpr std::size_t kDefaultArenaBytes = 380U * 1024U;
constexpr std::size_t kAlignment = alignof(std::max_align_t);
constexpr std::uint64_t kAllocationMagic = 0x4350484541503032ULL;

std::size_t alignUp(const std::size_t value, const std::size_t alignment) {
  if (alignment == 0) return value;
  const std::size_t remainder = value % alignment;
  return remainder == 0 ? value : value + (alignment - remainder);
}

struct alignas(std::max_align_t) BlockHeader {
  std::size_t size;
  std::size_t prevSize;
  BlockHeader* prevFree;
  BlockHeader* nextFree;
  bool isFree;
};

struct alignas(std::max_align_t) AllocationHeader {
  std::uint64_t magic;
  BlockHeader* block;
  std::size_t requested;
};

static_assert(sizeof(BlockHeader) % kAlignment == 0);
static_assert(sizeof(AllocationHeader) % kAlignment == 0);

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
//      the *real* host heap, never the 380KB arena, so the reported numbers are
//      unchanged whether tracing is on or off.
//   2. A thread-local reentrancy guard routes any allocation made *inside* the
//      tracer to the real heap. The arena runs under a non-recursive mutex and
//      backtrace()/symbol resolution allocate, so without this guard we would
//      deadlock or recurse.
// ---------------------------------------------------------------------------

// Depth incremented while we are inside tracer bookkeeping. The malloc wrappers
// and operator new check it and fall back to the real heap when it is non-zero.
thread_local int gTracerDepth = 0;

struct TracerGuard {
  TracerGuard() { ++gTracerDepth; }
  ~TracerGuard() { --gTracerDepth; }
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

struct TraceRecord {
  std::size_t requested;
  int frameCount;
  void* frames[kMaxTraceFrames];
};

template <class K, class V>
using RealMap = std::unordered_map<K, V, std::hash<K>, std::equal_to<K>, RealAllocator<std::pair<const K, V>>>;

class TraceRegistry {
 public:
  void setEnabled(const bool enabled) { enabled_ = enabled; }
  bool enabled() const { return enabled_; }

  void record(void* ptr, const std::size_t size) {
#if SIM_HEAP_TRACE_AVAILABLE
    if (!enabled_ || !ptr) return;
    TraceRecord rec;
    rec.requested = size;
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

 private:
  bool enabled_ = false;
  std::mutex mutex_;
  RealMap<void*, TraceRecord> live_;
};

TraceRegistry gTraceRegistry;

class ArenaAllocator {
 public:
  bool initialize(const std::size_t requestedBytes, const bool logInit) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) return true;

    const std::size_t alignedBytes = alignUp(requestedBytes, kAlignment);
    if (alignedBytes < minimumArenaBytes()) {
      errno = ENOMEM;
      return false;
    }

    void* slab = __real_malloc(alignedBytes);
    if (!slab) return false;

    slab_ = static_cast<std::byte*>(slab);
    slabBytes_ = alignedBytes;
    freeList_ = reinterpret_cast<BlockHeader*>(slab_);
    freeList_->size = slabBytes_;
    freeList_->prevSize = 0;
    freeList_->prevFree = nullptr;
    freeList_->nextFree = nullptr;
    freeList_->isFree = true;
    minFreeBytes_ = allocatableBytes(freeList_, kAlignment);
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
    if (bytes <= slab_ || bytes > slab_ + slabBytes_) return false;
    const AllocationHeader* allocation = static_cast<const AllocationHeader*>(ptr) - 1;
    return allocation->magic == kAllocationMagic;
  }

  void* allocate(std::size_t size, std::size_t alignment = kAlignment) {
    std::lock_guard<std::mutex> lock(mutex_);
    return allocateLocked(size, alignment);
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

    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) {
      errno = ENOMEM;
      return nullptr;
    }

    AllocationHeader* allocation = allocationHeaderFromUser(ptr);
    if (!allocation || allocation->magic != kAllocationMagic) {
      errno = EINVAL;
      return nullptr;
    }

    BlockHeader* block = allocation->block;
    const std::size_t oldRequested = allocation->requested;
    const std::size_t newUsedBytes = allocationBytesFor(block, size, alignment);
    if (newUsedBytes == 0) {
      errno = ENOMEM;
      return nullptr;
    }

    if (newUsedBytes <= block->size) {
      allocation->requested = size;
      splitBlockLocked(block, newUsedBytes);
      updateMinFreeLocked();
      gTraceRegistry.updateSize(ptr, size);
      return ptr;
    }

    BlockHeader* next = nextBlock(block);
    if (next && next->isFree && block->size + next->size >= newUsedBytes) {
      removeFreeBlock(next);
      block->size += next->size;
      BlockHeader* afterNext = nextBlock(block);
      if (afterNext) afterNext->prevSize = block->size;
      allocation->requested = size;
      splitBlockLocked(block, newUsedBytes);
      updateMinFreeLocked();
      gTraceRegistry.updateSize(ptr, size);
      return ptr;
    }

    void* newPtr = allocateLocked(size, alignment);
    if (!newPtr) return nullptr;

    std::memcpy(newPtr, ptr, oldRequested < size ? oldRequested : size);
    allocation->requested = oldRequested;
    freeLocked(ptr);
    return newPtr;
  }

  void deallocate(void* ptr) {
    if (!ptr) return;
    std::lock_guard<std::mutex> lock(mutex_);
    freeLocked(ptr);
  }

 private:
  static constexpr std::size_t minimumArenaBytes() {
    return sizeof(BlockHeader) + sizeof(AllocationHeader) + kAlignment + 1;
  }

  static AllocationHeader* allocationHeaderFromUser(void* userPtr) {
    return static_cast<AllocationHeader*>(userPtr) - 1;
  }

  static const AllocationHeader* allocationHeaderFromUser(const void* userPtr) {
    return static_cast<const AllocationHeader*>(userPtr) - 1;
  }

  static std::size_t allocatableBytes(const BlockHeader* block, const std::size_t alignment) {
    const uintptr_t blockStart = reinterpret_cast<uintptr_t>(block);
    const uintptr_t payloadStart = blockStart + sizeof(BlockHeader);
    const uintptr_t alignedUser =
        alignUp(payloadStart + sizeof(AllocationHeader), alignment);
    if (alignedUser < payloadStart + sizeof(AllocationHeader)) return 0;
    const uintptr_t usedStart = alignedUser - sizeof(AllocationHeader);
    if (usedStart < payloadStart) return 0;
    const std::size_t overhead = static_cast<std::size_t>((usedStart - blockStart) + sizeof(AllocationHeader));
    return block->size > overhead ? block->size - overhead : 0;
  }

  static std::size_t allocationBytesFor(const BlockHeader* block, const std::size_t requested,
                                        const std::size_t alignment) {
    const uintptr_t blockStart = reinterpret_cast<uintptr_t>(block);
    const uintptr_t payloadStart = blockStart + sizeof(BlockHeader);
    const uintptr_t alignedUser = alignUp(payloadStart + sizeof(AllocationHeader), alignment);
    if (alignedUser < payloadStart + sizeof(AllocationHeader)) return 0;
    if (requested > std::numeric_limits<std::size_t>::max() - alignedUser) return 0;
    const uintptr_t rawEnd = alignedUser + (requested == 0 ? 1 : requested);
    const std::size_t used = static_cast<std::size_t>(rawEnd - blockStart);
    return alignUp(used, kAlignment);
  }

  static std::size_t usedBytesFor(const BlockHeader* block, const AllocationHeader* header,
                                  const std::size_t requested) {
    const uintptr_t blockStart = reinterpret_cast<uintptr_t>(block);
    const uintptr_t user = reinterpret_cast<uintptr_t>(header + 1);
    const uintptr_t rawEnd = user + (requested == 0 ? 1 : requested);
    return alignUp(static_cast<std::size_t>(rawEnd - blockStart), kAlignment);
  }

  BlockHeader* nextBlock(BlockHeader* block) const {
    if (!block) return nullptr;
    std::byte* next = reinterpret_cast<std::byte*>(block) + block->size;
    return next >= slab_ + slabBytes_ ? nullptr : reinterpret_cast<BlockHeader*>(next);
  }

  BlockHeader* prevBlock(BlockHeader* block) const {
    if (!block || block->prevSize == 0) return nullptr;
    return reinterpret_cast<BlockHeader*>(reinterpret_cast<std::byte*>(block) - block->prevSize);
  }

  void addFreeBlock(BlockHeader* block) {
    block->isFree = true;
    block->prevFree = nullptr;
    block->nextFree = freeList_;
    if (freeList_) freeList_->prevFree = block;
    freeList_ = block;
  }

  void removeFreeBlock(BlockHeader* block) {
    if (block->prevFree) {
      block->prevFree->nextFree = block->nextFree;
    } else if (freeList_ == block) {
      freeList_ = block->nextFree;
    }
    if (block->nextFree) block->nextFree->prevFree = block->prevFree;
    block->prevFree = nullptr;
    block->nextFree = nullptr;
    block->isFree = false;
  }

  void splitBlockLocked(BlockHeader* block, const std::size_t usedBytes) {
    if (block->size <= usedBytes) return;

    const std::size_t remainder = block->size - usedBytes;
    if (remainder < minimumArenaBytes()) return;

    block->size = usedBytes;
    BlockHeader* remainderBlock = reinterpret_cast<BlockHeader*>(reinterpret_cast<std::byte*>(block) + usedBytes);
    remainderBlock->size = remainder;
    remainderBlock->prevSize = usedBytes;
    remainderBlock->prevFree = nullptr;
    remainderBlock->nextFree = nullptr;
    remainderBlock->isFree = true;
    BlockHeader* after = nextBlock(remainderBlock);
    if (after) after->prevSize = remainder;
    addFreeBlock(remainderBlock);
  }

  BlockHeader* coalesceLocked(BlockHeader* block) {
    BlockHeader* next = nextBlock(block);
    if (next && next->isFree) {
      removeFreeBlock(next);
      block->size += next->size;
      BlockHeader* after = nextBlock(block);
      if (after) after->prevSize = block->size;
    }

    BlockHeader* prev = prevBlock(block);
    if (prev && prev->isFree) {
      removeFreeBlock(prev);
      prev->size += block->size;
      BlockHeader* after = nextBlock(prev);
      if (after) after->prevSize = prev->size;
      block = prev;
    }

    return block;
  }

  void updateMinFreeLocked() {
    const std::size_t currentFree = freeBytesLocked();
    if (currentFree < minFreeBytes_) minFreeBytes_ = currentFree;
  }

  std::size_t freeBytesLocked() const {
    std::size_t total = 0;
    for (BlockHeader* block = freeList_; block; block = block->nextFree) {
      total += allocatableBytes(block, kAlignment);
    }
    return total;
  }

  std::size_t largestFreeBlockBytesLocked() const {
    std::size_t largest = 0;
    for (BlockHeader* block = freeList_; block; block = block->nextFree) {
      const std::size_t candidate = allocatableBytes(block, kAlignment);
      if (candidate > largest) largest = candidate;
    }
    return largest;
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
    std::size_t totalFree = 0;
    std::array<std::size_t, 5> histogram = {0, 0, 0, 0, 0};

    for (BlockHeader* block = freeList_; block; block = block->nextFree) {
      const std::size_t bytes = allocatableBytes(block, kAlignment);
      if (numBlocks < kMaxBlocks) {
        blocks[numBlocks++] = bytes;
      } else {
        numBlocks++;
      }
      totalFree += bytes;
      if (bytes < 1024) {
        histogram[0]++;
      } else if (bytes < 4096) {
        histogram[1]++;
      } else if (bytes < 16384) {
        histogram[2]++;
      } else if (bytes < 65536) {
        histogram[3]++;
      } else {
        histogram[4]++;
      }
    }

    const std::size_t sortCount = std::min(numBlocks, kMaxBlocks);
    std::sort(blocks.begin(), blocks.begin() + sortCount, std::greater<>());

    std::fprintf(stderr,
                 "[SIM] free list%s%s: blocks=%zu free=%zu max_alloc=%zu fragmentation=%zu%% histogram(<1K=%zu "
                 "1-4K=%zu 4-16K=%zu 16-64K=%zu 64K+=%zu)\n",
                 reason ? " " : "", reason ? reason : "", numBlocks, totalFree, largestFreeBlockBytesLocked(),
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

    const std::size_t effectiveAlignment = alignment < kAlignment ? kAlignment : alignment;
    if ((effectiveAlignment & (effectiveAlignment - 1U)) != 0) {
      errno = EINVAL;
      return nullptr;
    }

    for (BlockHeader* block = freeList_; block; block = block->nextFree) {
      const std::size_t usedBytes = allocationBytesFor(block, requestedSize, effectiveAlignment);
      if (usedBytes == 0 || usedBytes > block->size) continue;

      removeFreeBlock(block);
      splitBlockLocked(block, usedBytes);

      const uintptr_t blockStart = reinterpret_cast<uintptr_t>(block);
      const uintptr_t payloadStart = blockStart + sizeof(BlockHeader);
      const uintptr_t alignedUser = alignUp(payloadStart + sizeof(AllocationHeader), effectiveAlignment);
      AllocationHeader* allocation = reinterpret_cast<AllocationHeader*>(alignedUser - sizeof(AllocationHeader));
      allocation->magic = kAllocationMagic;
      allocation->block = block;
      allocation->requested = requestedSize;

      updateMinFreeLocked();
      void* userPtr = static_cast<void*>(allocation + 1);
      gTraceRegistry.record(userPtr, requestedSize);
      return userPtr;
    }

    errno = ENOMEM;
    return nullptr;
  }

  void freeLocked(void* ptr) {
    AllocationHeader* allocation = allocationHeaderFromUser(ptr);
    if (!allocation || allocation->magic != kAllocationMagic) return;

    gTraceRegistry.forget(ptr);
    BlockHeader* block = allocation->block;
    allocation->magic = 0;
    block = coalesceLocked(block);
    addFreeBlock(block);
  }

  void shutdownLocked() {
    if (!initialized_) return;
    __real_free(slab_);
    slab_ = nullptr;
    slabBytes_ = 0;
    freeList_ = nullptr;
    minFreeBytes_ = 0;
    initialized_ = false;
  }

  mutable std::mutex mutex_;
  std::byte* slab_ = nullptr;
  std::size_t slabBytes_ = 0;
  BlockHeader* freeList_ = nullptr;
  std::size_t minFreeBytes_ = 0;
  bool initialized_ = false;
};

ArenaAllocator gAllocator;
bool gArenaActive = false;

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
bool isSystemFile(const std::string& file) { return file.find("/usr/include") != std::string::npos; }

#if SIM_HEAP_TRACE_BACKWARD
// Resolve a stored allocation stack with backward-cpp and render it through
// backward's Printer (function + file:line + source snippet), skipping the
// leading allocator frames. Runs under the caller's TracerGuard.
void printSiteStack(const TraceRecord& rec) {
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
  // We buffer each frame below (see loop), which defeats color_mode::automatic's
  // tty probe on the buffer, so decide color once from the real stderr instead.
  printer.color_mode = isatty(fileno(stderr)) ? backward::ColorMode::always : backward::ColorMode::never;

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
    std::fputs(bodyStart == std::string::npos ? frame.c_str() : frame.c_str() + bodyStart + 1, stderr);
  }
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
#endif  // SIM_HEAP_TRACE_AVAILABLE

void TraceRegistry::dump(const char* reason) {
#if SIM_HEAP_TRACE_AVAILABLE
  if (!enabled_) return;

  // Everything below allocates (aggregation maps, strings, backward-cpp). The
  // arena is full at OOM time, so route all of it to the real heap.
  TracerGuard guard;
  std::lock_guard<std::mutex> lock(mutex_);

  struct Site {
    std::size_t bytes = 0;
    std::size_t count = 0;
    const TraceRecord* sample = nullptr;
  };

  std::unordered_map<std::uint64_t, Site> sites;
  std::size_t liveBytes = 0;
  std::size_t liveCount = 0;
  for (const auto& [ptr, rec] : live_) {
    std::uint64_t hash = 1469598103934665603ULL;  // FNV-1a over the frame addresses
    for (int i = 0; i < rec.frameCount; i++) {
      hash ^= reinterpret_cast<std::uintptr_t>(rec.frames[i]);
      hash *= 1099511628211ULL;
    }
    Site& site = sites[hash];
    site.bytes += rec.requested;
    site.count += 1;
    site.sample = &rec;
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
    printSiteStack(*site.sample);
  }
  std::fprintf(stderr, "[SIM] ===== end heap allocation trace =====\n\n");
#else
  (void)reason;
#endif
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
}

}  // namespace

namespace SimulatorHeap {

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

void dumpLiveAllocations(const char* reason) { gTraceRegistry.dump(reason ? reason : "manual"); }

void dumpFreeList(const char* reason) { gAllocator.dumpFreeList(reason ? reason : "manual"); }

#ifdef SIMULATOR_HEAP_TESTING
bool resetForTests(const std::size_t arenaBytes) {
  gAllocator.shutdown();
  return gAllocator.initialize(arenaBytes, false);
}

void shutdownForTests() { gAllocator.shutdown(); }

void* allocateForTests(const std::size_t size) { return gAllocator.allocate(size); }

void* callocForTests(const std::size_t nmemb, const std::size_t size) { return gAllocator.calloc(nmemb, size); }

void* reallocForTests(void* ptr, const std::size_t size) { return gAllocator.reallocate(ptr, size); }

void freeForTests(void* ptr) { gAllocator.deallocate(ptr); }
#endif

}  // namespace SimulatorHeap

#ifndef SIMULATOR_HEAP_TESTING_NO_GLOBAL_OVERRIDES
extern "C" void __wrap_free(void* ptr);

extern "C" void* __wrap_malloc(std::size_t size) {
  // gTracerDepth > 0: allocation made from inside the heap tracer itself; route
  // to the real heap so it neither consumes nor deadlocks on the arena.
  if (!gArenaActive || gTracerDepth > 0) return __real_malloc(size);
  void* ptr = gAllocator.allocate(size);
  if (!ptr) logHeapFailure("malloc", size, kAlignment, errno);
  return ptr;
}

extern "C" void* __wrap_calloc(std::size_t nmemb, std::size_t size) {
  if (!gArenaActive || gTracerDepth > 0) return __real_calloc(nmemb, size);
  void* ptr = gAllocator.calloc(nmemb, size);
  if (!ptr) {
    const std::size_t requested = (nmemb != 0 && size > std::numeric_limits<std::size_t>::max() / nmemb) ? 0 : nmemb * size;
    logHeapFailure("calloc", requested, kAlignment, errno);
  }
  return ptr;
}

extern "C" void* __wrap_realloc(void* ptr, std::size_t size) {
  if (!gArenaActive || gTracerDepth > 0) return __real_realloc(ptr, size);
  if (ptr && !arenaOwnsPointer(ptr)) return __real_realloc(ptr, size);
  void* newPtr = gAllocator.reallocate(ptr, size);
  if (!newPtr && size != 0) logHeapFailure("realloc", size, kAlignment, errno);
  return newPtr;
}

extern "C" void __wrap_free(void* ptr) {
  if (!ptr) return;
  if (!gArenaActive || !arenaOwnsPointer(ptr)) {
    __real_free(ptr);
    return;
  }
  gAllocator.deallocate(ptr);
}

void* operator new(std::size_t size) {
  if (void* ptr = __wrap_malloc(size)) return ptr;
  throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
  if (void* ptr = __wrap_malloc(size)) return ptr;
  throw std::bad_alloc();
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return __wrap_malloc(size); }

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { return __wrap_malloc(size); }

void* operator new(std::size_t size, std::align_val_t alignment) {
  const std::size_t requestedAlignment = static_cast<std::size_t>(alignment);
  if (!gArenaActive || gTracerDepth > 0) {
    if (void* ptr = hostAlignedAllocate(size, requestedAlignment)) return ptr;
    throw std::bad_alloc();
  }
  if (void* ptr = gAllocator.allocate(size, requestedAlignment)) return ptr;
  logHeapFailure("aligned operator new", size, requestedAlignment, errno);
  throw std::bad_alloc();
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
  const std::size_t requestedAlignment = static_cast<std::size_t>(alignment);
  if (!gArenaActive || gTracerDepth > 0) {
    if (void* ptr = hostAlignedAllocate(size, requestedAlignment)) return ptr;
    throw std::bad_alloc();
  }
  if (void* ptr = gAllocator.allocate(size, requestedAlignment)) return ptr;
  logHeapFailure("aligned operator new[]", size, requestedAlignment, errno);
  throw std::bad_alloc();
}

void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
  const std::size_t requestedAlignment = static_cast<std::size_t>(alignment);
  if (!gArenaActive || gTracerDepth > 0) return hostAlignedAllocate(size, requestedAlignment);
  void* ptr = gAllocator.allocate(size, requestedAlignment);
  if (!ptr) logHeapFailure("aligned operator new nothrow", size, requestedAlignment, errno);
  return ptr;
}

void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
  const std::size_t requestedAlignment = static_cast<std::size_t>(alignment);
  if (!gArenaActive || gTracerDepth > 0) return hostAlignedAllocate(size, requestedAlignment);
  void* ptr = gAllocator.allocate(size, requestedAlignment);
  if (!ptr) logHeapFailure("aligned operator new[] nothrow", size, requestedAlignment, errno);
  return ptr;
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
