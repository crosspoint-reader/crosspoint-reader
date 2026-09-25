#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// HarfBuzz types are opaque C structs; forward-declared so this header does not
// drag hb.h into every font consumer.
struct hb_face_t;
struct hb_font_t;
struct hb_buffer_t;

// OpenType shaping of complex-script runs (Bengali) for one font face.
//
// shape() rewrites each run as ShapingTokens.h tokens — glyph IDs with their
// HarfBuzz advances and mark offsets — and copies all other text unchanged, so
// the renderer draws conjuncts, reph and GPOS-positioned marks exactly as the
// font designs them.
//
// Memory: the layout tables (typically 10-40 KB for a Bengali face) and
// HarfBuzz's face/plan state load on the first run that needs them and stay
// until release(). Shapers whose sources carry the same font share one
// HarfBuzz face — every size of a family shapes from a single copy of its
// tables — and keep only a small per-size font object of their own. Every
// HarfBuzz allocation is budgeted (setMemoryBudget); on a failed allocation
// shape() returns false and the caller renders the unshaped fallback instead
// of aborting.
//
// Thread safety: all shapers share one lock, so shape() and release() may be
// called from any task.
class ComplexShaper {
 public:
  // A whole layout font (.cpfont shaping section). `release(data)` runs when
  // HarfBuzz drops it; nullptr means `data` came from allocate() and is freed
  // with deallocate(). Borrowed memory (e.g. flash-mapped) supplies its own.
  struct Blob {
    const uint8_t* data = nullptr;
    uint32_t length = 0;
    void (*release)(void* data) = nullptr;
  };
  // Fills `out` and returns true, or returns false when the blob is unavailable.
  using BlobLoader = bool (*)(void* ctx, Blob* out);
  // Loads one sfnt table by tag (TTF/OTF files). Same ownership contract.
  using TableLoader = uint8_t* (*)(void* ctx, uint32_t tag, uint32_t* length);

  ComplexShaper();
  ~ComplexShaper();
  ComplexShaper(const ComplexShaper&) = delete;
  ComplexShaper& operator=(const ComplexShaper&) = delete;

  // Configure exactly one source before the first shape(). `ctx` must outlive
  // the shaper (or the next setter call); it is only used while the face is
  // being built. `contentKey` identifies the blob's content (0 = unknown, never
  // shared); table sources are identified by their 'head' table.
  void setBlobSource(BlobLoader loader, void* ctx, uint32_t contentKey);
  void setTableSource(TableLoader loader, void* ctx);
  // Pixels per em in 26.6 fixed point: the size positions are produced at.
  void setScale(uint32_t ppem26_6);

  bool hasSource() const { return blobLoader_ != nullptr || tableLoader_ != nullptr; }

  // Rewrites every complex-script run in `utf8` as tokens and copies other
  // text unchanged. Returns false (out unspecified) when the text has no run
  // or the font could not be shaped with; the caller then draws `utf8` as is.
  bool shape(const char* utf8, std::string& out);

  // Frees the HarfBuzz objects and loaded tables. They are rebuilt on demand.
  void release();

  // True when `utf8` contains a codepoint that shape() would rewrite. A byte
  // scan (U+0980-U+09FF encode as E0 A6 xx / E0 A7 xx); dandas alone need no
  // shaping, next to Bengali they join its run.
  static bool containsComplexScript(const char* utf8) {
    if (utf8 == nullptr) return false;
    for (const auto* p = reinterpret_cast<const unsigned char*>(utf8); *p; ++p) {
      if (p[0] == 0xE0 && (p[1] == 0xA6 || p[1] == 0xA7)) return true;
    }
    return false;
  }

  // Budgeted allocation shared with HarfBuzz and the source loaders.
  static void* allocate(size_t size);
  static void deallocate(void* ptr);

  struct MemoryStats {
    size_t current = 0;
    size_t peak = 0;
    size_t budget = 0;
    uint32_t failures = 0;
    uint32_t shapedRuns = 0;  // runs HarfBuzz shaped
    uint32_t reusedRuns = 0;  // runs served from the cache or layout memo
  };
  static MemoryStats memoryStats();
  static void setMemoryBudget(size_t bytes);

  // Drops the shared shaped-run cache (rebuilt on demand).
  static void releaseCache();

  // release() on every live shaper plus releaseCache(): the memory-pressure
  // sink. Returns the bytes freed.
  static size_t releaseAll();

  // Layout memo: between beginMemo() and endMemo() every shaped run is also
  // kept in a scope-sized arena, so a paragraph that is measured and then
  // shaped again for its page cache runs HarfBuzz once per distinct run.
  // Scopes nest; the memo is freed when the outermost one ends.
  static void beginMemo();
  static void endMemo();

 private:
  bool ensureFont();
  bool appendShapedRun(const char* run, size_t length, std::string& out);
  void releaseLocked();

  BlobLoader blobLoader_ = nullptr;
  TableLoader tableLoader_ = nullptr;
  void* sourceCtx_ = nullptr;
  uint32_t scale26_6_ = 0;

  uint32_t contentKey_ = 0;

  struct SharedFace* face_ = nullptr;
  hb_font_t* font_ = nullptr;
  hb_buffer_t* buffer_ = nullptr;
  // After a failed build, skip this many shape() calls before retrying so a
  // persistently tight heap does not re-read the source for every word.
  uint16_t retryBackoff_ = 0;
  bool unusable_ = false;  // the source parsed but cannot shape (no glyphs)

  // Intrusive list of live shapers for releaseAll().
  ComplexShaper* prev_ = nullptr;
  ComplexShaper* next_ = nullptr;
};
