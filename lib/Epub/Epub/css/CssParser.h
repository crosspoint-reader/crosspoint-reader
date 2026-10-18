#pragma once

#include <HalStorage.h>

#include <list>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "CssStyle.h"

class BuildArena;  // lib/Memory — optional backing store for the disk index (Phase 2)

/**
 * Lightweight CSS parser for EPUB stylesheets
 *
 * Parses CSS files and extracts styling information relevant for e-ink display.
 * Uses a two-phase approach: first tokenizes the CSS content, then builds
 * a rule database that can be queried during HTML parsing.
 *
 * Supported selectors:
 *   - Element selectors: p, div, h1, etc.
 *   - Class selectors: .classname
 *   - Combined: element.classname
 *   - Compounds: .a.b, p.a.b, #id.a, p#id.a (up to 7 classes), and the universal *
 *   - Descendant and child combinators between compounds: blockquote p, div.poem > p
 *   - Grouped: selector1, selector2 { }
 *
 * Not supported (silently ignored):
 *   - Sibling combinators (+, ~) and attribute selectors
 *   - Pseudo-classes and pseudo-elements
 *   - Media queries (content is skipped)
 *   - @import and the other at-rules
 *
 * @font-face rules are not styles: they are collected into a side file next to the rule cache
 * (fontFaces / loadFontFaces), for the book's embedded fonts.
 */
class CssParser {
 public:
  struct ResolveStats {
    uint32_t resolveCalls = 0;
    uint32_t lowHeapSkips = 0;
    uint32_t lowHeapRescuedHits = 0;
    uint32_t lowHeapDiskBypasses = 0;
    uint32_t mapHits = 0;
    uint32_t hotHits = 0;
    uint32_t diskHits = 0;
    uint32_t misses = 0;
    uint32_t negativeHits = 0;
  };

  // True when the whole ruleset (index + style pool) is resident in arena memory, so every
  // lookupRules() is served without touching SD. Callers use this to tell a REAL low-heap
  // degradation from a harmless one: under heap pressure resolveStyle() still counts a
  // lowHeapSkip and passes allowDiskLookup=false, but the resident branch in lookupRules()
  // returns before that flag is ever read — the styles come back complete either way.
  // Treating those skips as degradation discards correct work (measured on X3: a 14-page
  // background build thrown away, forcing the released-path rebuild that then fails its
  // framebuffer realloc).
  bool isArenaResident() const { return arenaResident_ != nullptr; }

  // Bump when CSS cache format or rules change; section caches are invalidated when this changes
  // v8: v7 builds wrote saveToCache() payloads without the cssFloat byte the reader
  //     expects — bump invalidates those malformed caches.
  // v9: header gains a 4-byte indexOffset field; a pre-sorted {hash,offset} array is
  //     appended after the rule payloads so ensureCacheIndexLoaded() can bulk-read the
  //     entire index in one seek instead of scanning every selector record individually.
  // v10: indexOffset field removed; the sorted index is placed immediately after the
  //      11-byte header (before rule payloads), so ensureCacheIndexLoaded() reads header
  //      + index sequentially from file position 0 — no seek over the 37KB rule block.
  // v11: style payload gains a trailing smallCaps byte (font-variant: small-caps).
  // v12: ID selectors (#id, tag#id) are now stored and resolved.
  // v13: style payload gains fontSizeMultiplier (float + defined byte). Before this,
  //      font-size from stylesheets was parsed but silently DROPPED on device because
  //      every rule round-trips through this cache (only inline style="" survived).
  // v14: style payload gains a block-flags byte persisting listStyleNone and
  //      pageBreakBefore/After — the same dropped-through-the-cache defect as v13, so
  //      stylesheet-driven list markers and page breaks now actually reach the parser.
  // v15: rules with no renderer-supported declarations are omitted from the cache.
  // v16: img width/height record an explicit `auto` (and honour !important), so a later
  //      `height: auto` in the cascade clears an earlier length instead of being dropped.
  // v17: a flags byte after the header counters, carrying hasIdSelectors. Without it the
  //      "does any rule match on #id" question can only be answered while PARSING the CSS, and
  //      the normal path loads a compiled cache instead — so every id-bearing element paid two
  //      guaranteed-miss rule lookups on every book.
  // v18: `!important` is stripped from every declaration value, not only the dozen properties
  //      that remembered to do it, so margins, text-align, text-indent and the font/text
  //      shorthands no longer drop a declaration that carries the marker.
  // v19: `color`/`-webkit-text-fill-color: transparent` (and alpha-zero colours), `opacity: 0`
  //      and `visibility: hidden` are parsed and persisted (one visibility-flags byte). A cache
  //      compiled by v18 never saw `color`, so it is stale for exactly the books this is for --
  //      the PDF-to-EPUB conversions whose OCR layer is transparent text under a scan.
  // v20: `background-image` (and the url in a `background` shorthand) is recorded in a side file
  //      next to the rule cache (see backgroundImageFor). A v19 compile never looked for it, so
  //      a book whose picture is a CSS background (Alice's rabbit hole) kept showing none.
  // v21: `display` keeps Inline apart from Block (#388). A v20 cache stored display:inline as
  //      Block, which would now lay those spans out as blocks.
  // v22: every rule record carries its source order (u16 after the selector), and a selector
  //      declared again further down is kept as a second record instead of merged into the first:
  //      rules now apply by specificity, then stylesheet order (see resolveStyle).
  // v23: compound selectors (.a.b, p#id.a, *): the header gains the byte count of the complex-rule
  //      table, which follows it, before the sorted index (see complexTable).
  // v24: page-break-before/after (and break-*) `auto`, `avoid` and `avoid-page` are kept as an explicit
  //      "no break", so a more specific or later rule cancels an `always`; v23 dropped them.
  // v25: descendant and child selectors: a complex rule is a chain of compounds (see complexTable).
  // v26: `inherit` for font-weight, font-style and text-align is kept as an explicit marker that
  //      beats lower rules and then leaves the element to its parent; v25 stored it as normal/left.
  // v27: font-size records whether it is relative to the parent (em, %) or to the body (rem, px, pt,
  //      keywords), so nested rem and px sizes stop compounding; v26 kept only the multiplier.
  // v28: `font-family` is parsed and persisted (its first family, hashed; see fontFamilyHash), and
  //      @font-face rules are recorded in a side file (see loadFontFaces). A v27 compile never
  //      looked at either, so a book's embedded fonts would stay unknown.
  // v29: `font-family` also keeps the list's generic serif or sans-serif (CssFontFamily), which v28
  //      dropped.
  static constexpr uint8_t CSS_CACHE_VERSION = 29;
  // Bytes before the complex-rule table and the sorted offset index: version(1) + ruleCount(2) +
  // totalSelectorCandidates(4) + unsupportedSelectorSkips(4) + flags(1) + complexTableBytes(2).
  static constexpr uint32_t CSS_CACHE_HEADER_BYTES = 14;

  // Retained RAM per rule in disk-backed lookup mode (the sorted SelectorEntry index).
  // Heap gates (Section::heapAllowsEmbeddedStyle) size their contiguous-block floor
  // from this; a static_assert in CssParser.cpp pins it to sizeof(SelectorEntry).
  static constexpr size_t CSS_INDEX_BYTES_PER_RULE = 8;

  explicit CssParser(std::string cachePath) : cachePath(std::move(cachePath)) {}
  ~CssParser() = default;

  // Non-copyable
  CssParser(const CssParser&) = delete;
  CssParser& operator=(const CssParser&) = delete;

  /**
   * Load and parse CSS from a file stream.
   * Can be called multiple times to accumulate rules from multiple stylesheets.
   * @param source Open file handle to read from
   * @return true if parsing completed (even if no rules found)
   */
  bool loadFromStream(FsFile& source);

  /**
   * Look up the style for an HTML element, considering tag name, class, and id attributes.
   * Applies the CSS cascade: every matching rule, ordered by specificity (ids, then classes, then
   * the element name) and, between rules of equal specificity, by stylesheet order -- the later
   * rule wins. The order of the classes in the class attribute plays no part.
   *
   * @param tagName The HTML element name (e.g., "p", "div")
   * @param classAttr The class attribute value (may contain multiple space-separated classes)
   * @param idAttr The id attribute value (empty string if absent)
   * @return Combined style with all applicable rules merged
   */
  // True when at least one selector in the sheet matches on an element id (#id or tag#id). When
  // it is false an element's id cannot affect its style, so callers may ignore the id entirely --
  // which lets them share one cached style across every element with the same tag and class
  // instead of one per id. Persisted in the rule cache; see CSS_CACHE_VERSION v17.
  [[nodiscard]] bool hasIdSelectors() const { return hasIdSelectors_; }
  // True when the book's stylesheets had more rules than MAX_RULES: the cache holds the first
  // MAX_RULES and the rest are missing. Persisted in the cache flags byte (bit 1), so the book is
  // parsed once and says so on every open, instead of re-parsing on every open and hitting the
  // cap again (memory audit 2026-09, R4).
  [[nodiscard]] bool rulesTruncated() const { return rulesTruncated_; }
  // Selector census of the parsed (or loaded) sheets: selectors seen in rules with a supported
  // declaration, and how many of those the resolver could not use. Diagnostics and host tests.
  [[nodiscard]] uint32_t selectorCandidates() const { return totalSelectorCandidates_; }
  [[nodiscard]] uint32_t unsupportedSelectors() const { return unsupportedSelectorSkips_; }
  [[nodiscard]] uint16_t complexTableBytes() const { return complexBytes_; }

  // Descendant and child selectors (`blockquote p`, `div.poem > p`) match an element against its
  // open ancestors. A caller that walks the document keeps, for each open element, which of the
  // sheet's compounds it matches (compoundMask) and passes those of the element's ancestors. No
  // Ancestry (or depth 0): descendant and child rules do not match, everything else does.
  struct Ancestry {
    const uint64_t* masks = nullptr;  // outermost first; masks[depth - 1] is the parent
    size_t depth = 0;
    uint64_t self = 0;  // the element's own compoundMask
  };
  // True when the sheet has compound, descendant or child selectors, so compoundMask is worth
  // calling; false for most books, which then pay nothing for any of this.
  [[nodiscard]] bool hasComplexRules() const { return complexBytes_ > 0; }
  // Which of the sheet's compounds the element matches, one bit each; 0 without complex rules.
  [[nodiscard]] uint64_t compoundMask(const std::string& tagName, const std::string& classAttr,
                                      const std::string& idAttr) const;

  // dependsOnAncestors (optional) is set when a descendant or child rule could apply to the element:
  // the style then depends on where the element stands, and must not be reused for another element
  // with the same tag and class.
  [[nodiscard]] CssStyle resolveStyle(const std::string& tagName, const std::string& classAttr,
                                      const std::string& idAttr = {}, const Ancestry* ancestry = nullptr,
                                      bool* dependsOnAncestors = nullptr) const;

  /**
   * Parse an inline style attribute string.
   * @param styleValue The value of a style="" attribute
   * @return Parsed style properties
   */
  [[nodiscard]] static CssStyle parseInlineStyle(const std::string& styleValue);

  /**
   * Check if any rules have been loaded
   */
  [[nodiscard]] bool empty() const;

  /**
   * Get count of loaded rule sets
   */
  [[nodiscard]] size_t ruleCount() const;

  /**
   * Clear all loaded rules
   */
  void clear();

  /**
   * Clear per-section caches (hot/negative LRU) between section builds.
   * Retains the disk index in RAM if it fits within 10 KB, avoiding a cold SD
   * re-read (~240 ms) on the next section. Evicts if larger to protect heap.
   *
   * evictEverything=true is the defragmentation variant: it also drops the
   * retained disk index (lazily re-read from SD on the next resolve) and swaps
   * the unordered containers down so their bucket arrays are returned to the
   * heap — plain clear() keeps buckets allocated, and those small blocks can
   * pin the released-framebuffer hole a secondary-buffer realloc needs back.
   */
  void clearCaches(bool evictEverything = false);

  /**
   * Check if CSS rules cache file exists
   */
  bool hasCache() const;

  /**
   * Delete CSS rules cache file exists
   */
  void deleteCache() const;

  /**
   * Save parsed CSS rules to a cache file.
   * @return true if cache was written successfully
   */
  bool saveToCache() const;

  /**
   * Load CSS rules from a cache file.
   * Clears any existing rules before loading.
   * @return true if cache was loaded successfully
   */
  bool loadFromCache();

  // Low-memory CSS compilation pipeline:
  // - beginCacheCompile(): starts streaming compile mode
  // - appendCompiledFromStream(): parses one stylesheet stream into compile staging
  // - endCacheCompile(): finalizes cache file from staged records
  bool beginCacheCompile();
  bool appendCompiledFromStream(FsFile& source);
  bool endCacheCompile();
  // Abort a streaming compile (e.g. a stylesheet was skipped for low heap): discards the staged
  // temp file and persists NOTHING, so hasCache() stays false and the next open re-parses.
  // Persisting a partial compile is poison: an empty/incomplete index loads as VALID on every
  // later open, permanently stripping the book's styles until the cache is manually cleared.
  void abortCacheCompile();

  // CSS lookup telemetry helpers for tuning memory/caching behavior on-device.
  void resetResolveStats() const;
  [[nodiscard]] ResolveStats getResolveStats() const;
  void logResolveStats(const char* context) const;

  // Resident-arena memory breakdown (valid after a RESIDENT load; zero otherwise). Lets host
  // benchmarks and device logs quantify the CSS footprint and the dedup/compression win.
  struct ResidentFootprint {
    uint16_t ruleCount = 0;       // selectors in the index
    uint16_t distinctStyles = 0;  // pool entries after dedup
    uint32_t indexBytes = 0;      // sorted {hash, styleIdx} index
    uint32_t poolBytes = 0;       // distinct-style pool (compressed once compression lands)
    uint32_t totalBytes() const { return indexBytes + poolBytes; }
  };
  [[nodiscard]] ResidentFootprint getResidentFootprint() const;

  // Arena mode (docs/memory-allocation-strategy.md §4, class C). A section built with the
  // secondary framebuffer BORROWED (not freed) runs with ~52 KB less general heap than a
  // released build, so the resolver would otherwise self-degrade below MIN_FREE_HEAP_FOR_CSS
  // and its result gets discarded. These two knobs relocate the resolver's footprint into
  // the borrowed block instead:
  //   - setIndexArena(): the sorted disk index (SelectorEntry[]) is bump-allocated from the
  //     given arena rather than the heap vector. Pass nullptr to return to the heap vector.
  //     The arena owns the memory (reset per build); the parser only holds a view and drops
  //     it on clear()/clearCaches(), reloading lazily from disk on the next resolve.
  //   - setLeanResolve(true): skip the hot-rule LRU entirely (every lookup is index
  //     binary-search + one disk read) and use the lower LEAN_MIN_FREE_HEAP_FOR_CSS floor.
  //     With the index off-heap and no hot cache growth, the resolver's heap footprint is a
  //     few small strings, so the lower floor stays clear of the fault zone.
  // Both are reset by clear() so the shared per-epub instance never carries them into a
  // later heap-backed build or leaves a dangling arena view.
  void setIndexArena(BuildArena* arena) {
    indexArena_ = arena;
    arenaLoadAttempted_ = false;
  }
  void setLeanResolve(bool enable) { leanResolve_ = enable; }

  // Background images. The renderer cannot paint behind text, so an element's background picture
  // reaches the page as a picture of its own, placed after the element's content (see
  // ChapterHtmlSlimParser::noteBackgroundImage). It is not a CssStyle property: a path does not
  // fit the fixed-size style records the rule cache and the arena pool are built on, and almost
  // no book has one. Instead every rule that sets one records {selector, path} while the sheets
  // are compiled, and the list is kept in a small side file next to the rule cache (absent when
  // the book has none).
  //
  // Only a `no-repeat` background is recorded. A repeating one is a texture or a tile -- paper,
  // a border, a gradient strip -- and turned into a picture it would be a page of noise.
  struct BackgroundDeclaration {
    bool declared = false;  // a background / background-image declaration was seen
    std::string url;        // its url(), unresolved; empty for none, a colour, or a data: URI
    bool noRepeat = false;  // background-repeat (or the shorthand) said no-repeat
  };
  // Reads the background declarations of a rule body or a style attribute; the last one wins.
  [[nodiscard]] static BackgroundDeclaration parseBackground(std::string_view declarations);
  // The stylesheet about to be compiled, as a path inside the EPUB: url() resolves against its
  // directory. Set before each appendCompiledFromStream / loadFromStream.
  void setStylesheetPath(const std::string& sheetPathInEpub);
  // The background picture of an element as an EPUB path (directory of its sheet + url, not yet
  // normalised or unescaped), or nullptr. Same cascade as resolveStyle for the selectors it keeps
  // (tag, .class, tag.class, #id, tag#id): the most specific wins, then the later rule.
  [[nodiscard]] const std::string* backgroundImageFor(const std::string& tagName, const std::string& classAttr,
                                                      const std::string& idAttr = {}) const;

  // A book's embedded fonts: one @font-face rule each, in source order, recorded while the sheets
  // are compiled (at most MAX_FONT_FACES) and kept in a side file next to the rule cache, absent
  // when the book declares none. Read back as records (loadFontFaces), one face at a time
  // (nextFontFace): a face's text points into them.
  struct FontFace {
    uint32_t family = 0;    // fontFamilyHash() of the declared name
    uint16_t weight = 400;  // font-weight: 100-900, normal 400, bold 700
    bool italic = false;    // font-style other than normal
    std::string_view name;  // the declared family name as written, unquoted
    // The src url() the reader can best use, joined to its sheet's directory: an EPUB path, not
    // yet normalised or unescaped. TrueType first, then OpenType, then anything else; never
    // WOFF2, SVG, EOT, a data: URI or a remote font.
    std::string_view href;
  };
  static constexpr size_t MAX_FONT_FACES = 32;
  // A family name's identity, as font-family and @font-face compare names: case-insensitive, runs
  // of whitespace as one space, quotes removed. Never 0, which CssStyle::fontFamily keeps for the
  // reader's font, and its low two bits are 0 (CssFontFamily).
  [[nodiscard]] static uint32_t fontFamilyHash(std::string_view name);
  // The face records of the book whose cache is at `cachePath`, for nextFontFace. False when there
  // are none or the side file cannot be read.
  static bool loadFontFaces(const std::string& cachePath, std::string& records);
  // Takes the first face off `records`. False when none is left (or the rest is damaged).
  static bool nextFontFace(std::string_view& records, FontFace& face);

 private:
  // The faces recorded so far, as the side file stores them (see fontFacesFile), and their count.
  std::string fontFaces_;
  uint8_t fontFaceCount_ = 0;
  void parseFontFace(std::string_view declarations);
  void saveFontFaces() const;

  // See backgroundImageFor. An empty path records an explicit `none`, which cancels an earlier
  // rule's picture for the same selector.
  struct BackgroundImageRule {
    std::string selector;
    std::string path;
  };
  static constexpr size_t MAX_BACKGROUND_IMAGE_RULES = 32;
  mutable std::vector<BackgroundImageRule> backgroundImages_;
  mutable bool backgroundImagesLoaded_ = false;
  std::string stylesheetDir_;
  void recordBackgroundImage(std::string_view selectorGroup, const std::string& path);
  void saveBackgroundImages() const;
  void loadBackgroundImages() const;

  // A rule's place in the stylesheets: rules of equal specificity apply in this order. Every
  // selector of one rule block shares it, and it counts on across stylesheets, so a later sheet's
  // rules come after an earlier sheet's. Saturates (rule blocks beyond 65535 tie, which only a
  // pathological sheet reaches: MAX_RULES caps the stored rules far below it).
  uint16_t nextRuleOrder_ = 0;
  struct OrderedStyle {
    CssStyle style;
    uint16_t order = 0;
  };

  // Storage: maps normalized selector -> style properties. A multimap: a selector declared again
  // after other rules is kept as a second entry, because merging it into the first would move the
  // first's declarations past the rules in between (see processRuleBlockWithStyle).
  std::unordered_multimap<std::string, OrderedStyle> rulesBySelector_;

  std::string cachePath;

  // Disk-backed CSS dictionary index: FNV-1a hash of selector -> byte offset in cache file.
  // Flat sorted array — 8 bytes per entry vs ~60 bytes for unordered_map node.
  // At 1500 rules: ~12 KB instead of ~96 KB. Binary search on hot path.
  // The hash is only a candidate filter: lookups re-read the length-prefixed selector
  // at `offset` and compare it to the query, so a 32-bit collision costs one extra
  // disk read instead of resolving the wrong style.
  struct SelectorEntry {
    uint32_t hash;    // FNV-1a 32-bit of the normalized selector string
    uint32_t offset;  // byte offset of the rule record (selector + payload) in the cache file
  };
  static_assert(sizeof(SelectorEntry) == CSS_INDEX_BYTES_PER_RULE,
                "SelectorEntry size changed — update CSS_INDEX_BYTES_PER_RULE so heap gates stay calibrated");
  mutable bool cacheIndexLoaded_ = false;
  // Arena mode loads the ruleset exactly once, at the build's setup. A lookup that finds the
  // index missing afterwards must not reload it: the load reserves and commits a block, which
  // mid-parse would sit above the parser's open page block and be rewound with it (memory
  // audit 2026-09, L3) -- and a load that failed at setup would otherwise be retried, and
  // bump-allocate again, on every lookup.
  mutable bool arenaLoadAttempted_ = false;
  mutable size_t cachedRuleCount_ = 0;
  mutable std::vector<SelectorEntry> cacheRuleOffsets_;  // heap backing (indexArena_ == nullptr)
  mutable uint32_t totalSelectorCandidates_ = 0;
  mutable uint32_t unsupportedSelectorSkips_ = 0;

  // Phase-2 arena mode (see setIndexArena/setLeanResolve). When indexArena_ is set,
  // ensureCacheIndexLoaded() puts the ruleset in arena memory (not the heap vector) using
  // one of two layouts, decided by what fits — mirroring FreeInkBook's resident ruleset
  // (libs/book/FreeInkBook/src/css/Css.cpp) as far as witchhunt's ~108 B CssStyle allows:
  //   - RESIDENT (preferred): a sorted {hash, styleOff} index at arenaResident_ plus a pool of
  //     DISTINCT styles at arenaStylePool_, each SPARSE-COMPRESSED (only the fields the CSS
  //     'defined' mask flags — a real rule sets 1-4 of 24 properties, vs ~108 B for the full
  //     struct). Identical styles are deduplicated (Calibre emits hundreds of differently-named
  //     classes with byte-identical declarations). Compression + dedup together let a large or
  //     repetitive stylesheet that a flat {hash, CssStyle} array couldn't hold resolve in RAM
  //     with zero disk reads; styles decompress on lookup (build-time, ~microseconds).
  //   - INDEX-only (fallback when even the compressed pool won't fit): the sorted 8 B/rule
  //     SelectorEntry index at arenaIndex_; payloads are read from disk on demand.
  // All are non-owning views into arena memory (the arena resets per build); the parser
  // drops them on clear()/clearCaches() and reloads on the next resolve.
  struct ResidentEntry {
    uint32_t hash;      // FNV-1a of the normalized selector (same as SelectorEntry::hash)
    uint16_t styleOff;  // byte offset of this rule's compressed style within arenaStylePool_
    uint16_t order;     // the rule's source order (see nextRuleOrder_)
  };  // 8 B with no padding; a pool past 64 KB cannot be addressed and falls back to INDEX-only
  BuildArena* indexArena_ = nullptr;
  mutable ResidentEntry* arenaResident_ = nullptr;  // sorted by hash, cachedRuleCount_ entries
  mutable uint8_t* arenaStylePool_ = nullptr;       // distinct styles, sparse-compressed, length-prefixed
  mutable uint16_t arenaStyleCount_ = 0;            // distinct style records in the pool
  mutable uint32_t arenaPoolBytes_ = 0;             // bytes used by the compressed pool
  mutable SelectorEntry* arenaIndex_ = nullptr;
  bool leanResolve_ = false;

  // Stream the whole ruleset into the arena as a sorted {hash, styleIdx} index plus a pool of
  // distinct CssStyles (RESIDENT mode; identical styles deduplicated). Returns false if the
  // arena can't hold it (caller then tries INDEX-only).
  bool loadArenaResident(FsFile& file, uint16_t ruleCount, uint32_t totalCandidates, uint32_t unsupportedSkips) const;
  // Forget the current ruleset view (heap vector or either arena layout) so
  // ensureCacheIndexLoaded() reloads it.
  void dropIndex() const;

  static uint32_t selectorHash(std::string_view s);

  // Bounded hot cache of most recently used rules. Holds only selectors with a single rule: one
  // declared again further down has several, and is always read from the index.
  mutable std::list<std::string> hotRuleLru_;
  mutable std::unordered_map<std::string, std::pair<OrderedStyle, std::list<std::string>::iterator>> hotRuleCache_;
  mutable std::unordered_set<std::string> negativeRuleCache_;
  mutable ResolveStats resolveStats_;
  // Set while parsing when a selector matches on an id, and restored from the cache header
  // otherwise. Conservative default: assume ids matter until something says they do not.
  mutable bool hasIdSelectors_ = true;   // mutable: restored by the const cache-index load
  mutable bool rulesTruncated_ = false;  // see rulesTruncated(); restored by the cache load

  bool compileModeActive_ = false;
  bool compileModeFailed_ = false;
  std::string compileTempPath_;
  FsFile compileTempFile_;
  // The latest staged record of each selector (the one a redeclaration may merge into), and the
  // earlier records of selectors redeclared after other rules, which stay rules of their own.
  struct StagedRule {
    uint32_t offset;  // of the record in the staging file
    uint16_t order;
  };
  std::unordered_map<std::string, StagedRule> compileSelectorOffsets_;
  std::vector<uint32_t> compileRetiredOffsets_;

  // Reused scratch for the selector currently being processed (processRuleBlockWithStyle).
  // A member rather than a local so its buffer survives across selectors AND across rule
  // blocks: the parse then costs one growth for the whole stylesheet instead of a fresh heap
  // string per selector. Only valid inside processRuleBlockWithStyle.
  std::string selectorKeyBuf_;

  // Internal parsing helpers
  void processRuleBlockWithStyle(std::string_view selectorGroup, const CssStyle& style);
  static CssStyle parseDeclarations(std::string_view declBlock);
  static void parseDeclarationIntoStyle(std::string_view decl, CssStyle& style, std::string& propNameBuf,
                                        std::string& propValueBuf);

  // Individual property value parsers
  // Take string_view and do NOT normalize — every caller already passes normalized text, and
  // normalized() is idempotent. See the note above the definitions in CssParser.cpp.
  static CssTextAlign interpretAlignment(std::string_view val);
  static CssFontStyle interpretFontStyle(std::string_view val);
  static CssFontWeight interpretFontWeight(std::string_view val);
  static CssTextDecoration interpretDecoration(std::string_view val);
  static CssLength interpretLength(std::string_view val);
  /** Returns true only when a numeric length was parsed (e.g. 2em, 50%). False for auto/inherit/initial. */
  static bool tryInterpretLength(std::string_view val, CssLength& out);
  /** width:/height: on an image. Returns true when the declaration applies, with `out` holding
   *  either the length or the CssUnit::Auto marker; false for values we ignore. */
  static bool interpretImageSize(std::string_view val, CssLength& out);

  // String utilities
  static std::string normalized(const std::string& s);
  static void normalizedInto(std::string_view s, std::string& out);
  static std::vector<std::string> splitWhitespace(const std::string& s);

  // On-demand rule loading helpers
  bool ensureCacheIndexLoaded() const;
  // resolveStyle's accumulator: every matching rule is merged into it ranked by specificity, then
  // source order (CssStyle::applyOver with ranks), so lookups may run in any order.
  struct Cascade;
  // Merges every rule stored for `selector` into `cascade` at `specificity`; true when any matched.
  bool lookupRules(const std::string& selector, uint32_t specificity, Cascade& cascade,
                   bool allowDiskLookup = true) const;
  bool readRuleFromDiskAtOffset(uint32_t ruleOffset, const std::string& selector, CssStyle& outStyle,
                                uint16_t& outOrder) const;
  static bool readCssStylePayload(FsFile& file, CssStyle& style);
  static void writeCssStylePayload(FsFile& file, const CssStyle& style);
  // A rule record, the unit of the cache file and the compile staging file:
  // selectorLen(2) selector order(2) style payload.
  static void writeRuleRecord(FsFile& file, const std::string& selector, uint16_t order, const CssStyle& style);
  // complexTableBytes(2) and the complex-rule table, as the cache header ends.
  void writeComplexTable(FsFile& file) const;
  void touchHotRule(const std::string& selector) const;
  void cacheHotRule(const std::string& selector, const OrderedStyle& rule) const;

  // --- Complex rules (v23) ---
  // Selectors the hashed index cannot key -- a compound of several parts (.a.b, p.a.b, p#id.a), the
  // universal `*`, and (v25) descendant and child selectors -- live in a small table of their own,
  // matched by a scan per element. Only a sheet that has them pays: resolveStyle skips the scan
  // when the table is empty. The table is
  // held in RAM in every mode, built on the heap while parsing and loaded with the index (heap, or
  // the build arena), because it is small and capped (MAX_COMPLEX_BYTES), and resolving from it
  // must not cost a disk read per element. One contiguous block:
  //   compoundCount(1) ruleCount(1) compoundBytes(2)
  //   compounds: the distinct compounds, each flags(1) [tag(4)] [id(4)] class(4)...
  //              (flags 0x80 element name, 0x40 id, low 3 bits the class count; names are
  //              selectorHash of the lowercase name, classes sorted so equal compounds are equal
  //              bytes). An element matches a compound when it has every part; bit i of an
  //              element's compound mask says it matches compound i.
  //   rules:     order(2) specificity(2) steps(1) styleLen(1) step(steps) compressed style(styleLen)
  //              (v25) the steps are the selector's compounds right to left, the subject first:
  //              each the compound index (low 6 bits) and, from the second on, 0x40 when it must
  //              be the parent of the step before it (`>`) rather than any ancestor (a space).
  // The pointer is the heap block (parse or heap-mode load) or the arena copy; nullptr = no table.
  mutable std::unique_ptr<uint8_t[]> complexHeap_;
  mutable const uint8_t* complexArena_ = nullptr;
  mutable uint16_t complexBytes_ = 0;
  mutable uint16_t complexCapacity_ = 0;  // of complexHeap_ while parsing
  const uint8_t* complexTable() const { return complexArena_ ? complexArena_ : complexHeap_.get(); }
  // Adds a compound selector to the table; false (the selector is then unsupported) for anything
  // that is not one, or when a cap is reached.
  bool addComplexRule(std::string_view selector, const CssStyle& style, uint16_t order);
  bool growComplexTable(size_t needed);
  // The element's compound mask from its names, hashed like the table's (selectorHash of the
  // lowercase name); idHash is nullptr without an id.
  uint64_t matchCompounds(uint32_t tagHash, const uint32_t* idHash, const uint32_t* classHashes,
                          size_t classCount) const;
  // Merges the complex rules that match the element (its compound mask `self`, its ancestors in
  // `ancestry`) into `cascade`.
  void applyComplexRules(uint64_t self, const Ancestry* ancestry, Cascade& cascade, bool* dependsOnAncestors) const;
};
