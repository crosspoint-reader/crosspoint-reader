#pragma once

#include <IndicScripts.h>

#include <cstddef>
#include <cstdint>
#include <string>

// OpenType shaping of complex-script runs (the Indic scripts listed in
// IndicScripts.h) for one font face.
//
// shape() rewrites each run as ShapingTokens.h tokens — glyph IDs with their
// advances and mark offsets — and copies all other text unchanged, so the
// renderer draws conjuncts, reph and GPOS-positioned marks exactly as the
// font designs them. The shaping itself is lib/OtShaper, which reads the
// font's GSUB/GPOS tables in place.
//
// Memory: the layout tables (typically 5-80 KB for an Indic face) load on the
// first run that needs them and stay until release(); on boards without
// PSRAM they are memory-mapped from flash and cost no heap. Shapers whose
// sources carry the same font share one face — every size of a family shapes
// from a single copy of its tables — along with its shaping plan (the lookup
// list for the script, a few KB). Loaded tables count against a budget
// (setMemoryBudget); when it runs out shape() returns false and the caller
// renders the unshaped fallback.
//
// Thread safety: all shapers share one lock, so shape() and release() may be
// called from any task.
class ComplexShaper {
 public:
  // A whole layout font (.cpfont shaping section). `release(data)` runs when
  // the face drops it; nullptr means `data` came from allocate() and is freed
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
  // A glyph's advance in 26.6 pixels at the shaper's scale; negative when unknown.
  using AdvanceSource = int32_t (*)(void* ctx, uint32_t glyph);

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
  // For text drawn from another instance of the source's design (a variable
  // font's wght axis): glyphs advance by `source` plus what GPOS adjusts,
  // instead of by the source's hmtx. nullptr: the source's own advances.
  void setAdvanceSource(AdvanceSource source, void* ctx);

  bool hasSource() const { return blobLoader_ != nullptr || tableLoader_ != nullptr; }

  // Rewrites every complex-script run in `utf8` as tokens and copies other
  // text unchanged. A run is one script's text plus the shared codepoints
  // around it (dandas, joiners); runs of a script this font's layout tables
  // do not cover stay text. Returns false (out unspecified) when the text has
  // no run to shape or the font could not be shaped with; the caller then
  // draws `utf8` as is.
  bool shape(const char* utf8, std::string& out);

  // Frees the face, its plans and loaded tables. They are rebuilt on demand.
  void release();

  // True when `utf8` may contain a run shape() would rewrite: a byte scan
  // for the Indic blocks.
  static bool containsComplexScript(const char* utf8) { return indic::containsIndic(utf8); }

  // The language shaped text is written in (BCP 47, e.g. the book's
  // dc:language; empty = unknown). Fonts can draw a script differently per
  // language — Marathi and Nepali Devanagari differ from Hindi — through
  // their language systems. Forgets every cached run and plan when it
  // changes.
  static void setDocumentLanguage(const char* bcp47);

  // Budgeted allocation for the source loaders (layout tables in RAM).
  static void* allocate(size_t size);
  static void deallocate(void* ptr);
  // Loaders call this when their source cannot be loaded now (a read failed,
  // or it fits neither flash nor the budget), so the shaper retries later
  // instead of treating the source as malformed.
  static void noteSourceUnavailable();

  struct MemoryStats {
    size_t current = 0;
    size_t peak = 0;
    size_t budget = 0;
    uint32_t failures = 0;
    uint32_t shapedRuns = 0;  // runs the shaper shaped
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
  // shaped again for its page cache runs the shaper once per distinct run,
  // and a text that failed to shape fails again rather than shaping once
  // memory frees up. Scopes nest; the memo is freed when the outermost one
  // ends (releaseAll() leaves it alone).
  static void beginMemo();
  static void endMemo();

 private:
  enum class Coverage : uint8_t { Covered, NotCovered, Unknown };

  bool ensureFace();
  // ensureFace(); when that fails for lack of memory, drops every face and
  // tries once more.
  bool ensureFaceMakingRoom();
  static void releaseEveryFaceLocked();
  // Whether this font's layout tables map `script`; Unknown when the face
  // cannot be built right now.
  Coverage coverage(const indic::ScriptInfo& script);
  // shape() without the layout memo's record of failures.
  bool shapeRuns(const char* utf8, std::string& out);
  bool appendShapedRun(const char* run, size_t length, const indic::ScriptInfo& script, std::string& out);
  void releaseLocked();

  BlobLoader blobLoader_ = nullptr;
  TableLoader tableLoader_ = nullptr;
  // Set by setTableSource(), so only builds with table sources link the
  // table path and the runtime planner behind it.
  struct SharedFace* (*buildTableFace_)(TableLoader loader, void* ctx, uint32_t* key) = nullptr;
  void* sourceCtx_ = nullptr;
  AdvanceSource advanceSource_ = nullptr;
  void* advanceCtx_ = nullptr;
  uint32_t scale26_6_ = 0;

  uint32_t contentKey_ = 0;

  struct SharedFace* face_ = nullptr;
  // After a failed build, skip this many shape() calls, for at most a
  // second, before retrying: a persistently tight heap does not re-read the
  // source for every word, and a shaper left unused while memory came back
  // (pages drawn from the page cache) shapes the next word it gets.
  uint16_t retryBackoff_ = 0;
  uint32_t backoffStartMs_ = 0;
  bool unusable_ = false;  // the source was read but is malformed
  // One bit per indic::SCRIPTS entry. Coverage is a property of the source,
  // so it survives release().
  uint16_t coverageChecked_ = 0;
  uint16_t coverageMask_ = 0;
  static_assert(indic::SCRIPT_COUNT <= 16, "coverage bits");

  // Intrusive list of live shapers for releaseAll().
  ComplexShaper* prev_ = nullptr;
  ComplexShaper* next_ = nullptr;
};
