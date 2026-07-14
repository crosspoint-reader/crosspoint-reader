#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "CssStyle.h"

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
 *   - Grouped: selector1, selector2 { }
 *
 * Not supported (silently ignored):
 *   - Descendant/child selectors
 *   - Pseudo-classes and pseudo-elements
 *   - Media queries (content is skipped)
 *   - @import, @font-face, etc.
 *
 * Storage is deliberately compact: books converted per-chapter (e.g. Amazon
 * exports) can declare 1000+ class selectors whose bodies repeat a handful of
 * styles. Rules live in three flat pools — a sorted selector index, a selector
 * string pool, and a deduplicated style pool — costing ~22 bytes per rule
 * instead of ~144 bytes per std::unordered_map node, so entire books fit in
 * RAM that previously forced silent CSS truncation (issue #2591).
 */
class CssParser {
 public:
  // Bump when CSS cache format or rules change; section caches are invalidated when this changes
  static constexpr uint8_t CSS_CACHE_VERSION = 8;

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
  bool loadFromStream(HalFile& source);

  /**
   * Look up the style for an HTML element, considering tag name and class attributes.
   * Applies CSS cascade: element style < class style < element.class style
   *
   * @param tagName The HTML element name (e.g., "p", "div")
   * @param classAttr The class attribute value (may contain multiple space-separated classes)
   * @return Combined style with all applicable rules merged
   */
  [[nodiscard]] CssStyle resolveStyle(std::string_view tagName, std::string_view classAttr) const;

  /**
   * Parse an inline style attribute string.
   * @param styleValue The value of a style="" attribute
   * @return Parsed style properties
   */
  [[nodiscard]] static CssStyle parseInlineStyle(std::string_view styleValue);

  /**
   * Check if any rules have been loaded
   */
  [[nodiscard]] bool empty() const { return entryCount_ == 0; }

  /**
   * Get count of loaded rule sets
   */
  [[nodiscard]] size_t ruleCount() const { return entryCount_; }

  /**
   * Get count of distinct style bodies shared by the rules (diagnostics/tests)
   */
  [[nodiscard]] size_t uniqueStyleCount() const { return styleCount_; }

  /**
   * Clear all loaded rules and release their memory. Callers rely on this to
   * unpin the rule pools between section builds, so it must free, not just reset.
   */
  void clear() {
    entries_.reset();
    selectorPool_.reset();
    stylePool_.reset();
    styleHashes_.reset();
    entryCount_ = entryCapacity_ = 0;
    poolSize_ = poolCapacity_ = 0;
    styleCount_ = styleCapacity_ = 0;
  }

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

 private:
  // One rule in the selector index. 8 bytes with no padding; the in-memory
  // layout doubles as the cache wire layout (both targets are little-endian).
  struct SelectorEntry {
    uint32_t offset;    // byte offset of the case-folded selector in selectorPool_
    uint16_t styleIdx;  // index into stylePool_
    uint16_t length;    // selector byte length, 1..MAX_SELECTOR_LENGTH
  };
  static_assert(sizeof(SelectorEntry) == 8, "SelectorEntry is serialized raw; layout must stay 8 bytes");

  // Rule storage: entries_ is sorted by the case-folded selector bytes it
  // references in selectorPool_, enabling binary search. stylePool_ holds each
  // distinct style body once; entries share indices into it. styleHashes_ is a
  // build-time dedup prefilter parallel to stylePool_ (FNV-32 of the style's
  // canonical wire encoding).
  std::unique_ptr<SelectorEntry[]> entries_;
  std::unique_ptr<char[]> selectorPool_;
  std::unique_ptr<CssStyle[]> stylePool_;
  std::unique_ptr<uint32_t[]> styleHashes_;
  uint16_t entryCount_ = 0;
  uint16_t entryCapacity_ = 0;
  uint32_t poolSize_ = 0;
  uint32_t poolCapacity_ = 0;
  uint16_t styleCount_ = 0;
  uint16_t styleCapacity_ = 0;

  std::string cachePath;

  // Sorted-index primitives. Lookup keys are passed as up to three pieces
  // compared as if concatenated and case-folded (e.g. {".", cls}), so probes
  // never materialize a scratch string.
  [[nodiscard]] int compareEntryToPieces(const SelectorEntry& entry, std::string_view p0, std::string_view p1,
                                         std::string_view p2) const;
  [[nodiscard]] size_t lowerBound(std::string_view p0, std::string_view p1, std::string_view p2, bool& exact) const;
  [[nodiscard]] const CssStyle* findStyle(std::string_view p0, std::string_view p1 = {},
                                          std::string_view p2 = {}) const;

  // Pool builders. All grow via nothrow allocation and fail soft (skip rule).
  bool internStyle(const CssStyle& style, uint16_t& idxOut);
  bool appendSelector(std::string_view sel, uint32_t& offsetOut);
  bool ensureEntryCapacity(size_t needed);
  bool ensurePoolCapacity(size_t needed);
  bool ensureStyleCapacity(size_t needed);

  // Internal parsing helpers
  void processRuleBlockWithStyle(std::string_view selectorGroup, const CssStyle& style);
  static CssStyle parseDeclarations(std::string_view declBlock);
  static void parseDeclarationIntoStyle(std::string_view decl, CssStyle& style);

  // Individual property value parsers
  static CssTextAlign interpretAlignment(std::string_view val);
  static CssFontStyle interpretFontStyle(std::string_view val);
  static CssFontWeight interpretFontWeight(std::string_view val);
  static CssTextDecoration interpretDecoration(std::string_view val);
  static CssLength interpretLength(std::string_view val);
  /** Returns true only when a numeric length was parsed (e.g. 2em, 50%). False for auto/inherit/initial. */
  static bool tryInterpretLength(std::string_view val, CssLength& out);
};
