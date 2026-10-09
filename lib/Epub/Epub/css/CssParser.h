#pragma once

#include <HalStorage.h>

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
 *   - Compounds of an optional element, #id and up to 4 classes: p, .a, p.a.b, #id, p#id.a
 *   - Descendant and child combinators between compounds: div.poem p, blockquote > p
 *   - Grouped: selector1, selector2 { }
 * Matching rules are applied in specificity order (ids, then classes, then elements).
 *
 * Not supported (silently ignored):
 *   - Sibling combinators (+, ~), attribute selectors, pseudo-classes and pseudo-elements
 *   - Media queries (content is skipped)
 *   - @import, @font-face, etc.
 */
// Hashed identity of an open element, used to match descendant/child selectors.
struct CssAncestor {
  static constexpr uint8_t MAX_CLASSES = 4;  // classes past the fourth are not matchable
  uint32_t tagHash = 0;
  uint32_t idHash = 0;  // 0 when the element has no id
  uint32_t classHashes[MAX_CLASSES] = {};
  uint8_t classCount = 0;
};

class CssParser {
 public:
  enum class ParseResult : uint8_t {
    Complete,
    Partial,
    Error,
  };

  enum class CacheStatus : uint8_t {
    Missing,
    Complete,
    Partial,
    Invalid,
  };

  enum class CacheLoadResult : uint8_t {
    Complete,
    LowMemory,
    Invalid,
  };

  // Bump when CSS cache format or rules change; section caches are invalidated when this changes
  static constexpr uint8_t CSS_CACHE_VERSION = 17;

  explicit CssParser(std::string cachePath) : cachePath(std::move(cachePath)) {}
  ~CssParser() = default;

  // Non-copyable
  CssParser(const CssParser&) = delete;
  CssParser& operator=(const CssParser&) = delete;

  /**
   * Load and parse CSS from a file stream.
   * Can be called multiple times to accumulate rules from multiple stylesheets.
   * @param source Open file handle to read from
   * @return Complete unless bounded storage stopped rule growth or the source was invalid
   */
  ParseResult loadFromStream(HalFile& source);

  /**
   * Look up the style for an HTML element, merging every matching rule in specificity order.
   *
   * @param tagName The HTML element name (e.g., "p", "div")
   * @param classAttr The class attribute value (may contain multiple space-separated classes)
   * @param idAttr The id attribute value (may be empty)
   * @param ancestors Open ancestors, outermost first; the last entry is the direct parent
   * @param firstLetter Resolve the element's ::first-letter rules instead of its own
   * @return Combined style with all applicable rules merged
   */
  [[nodiscard]] CssStyle resolveStyle(std::string_view tagName, std::string_view classAttr,
                                      std::string_view idAttr = {}, const CssAncestor* ancestors = nullptr,
                                      size_t ancestorCount = 0, bool firstLetter = false) const;

  /** True when any ::first-letter rule is stored, so callers can skip that lookup. */
  [[nodiscard]] bool hasFirstLetterRules() const { return hasFirstLetterRules_; }

  [[nodiscard]] static CssAncestor makeAncestor(std::string_view tagName, std::string_view classAttr,
                                                std::string_view idAttr);

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
   * Clear all loaded rules
   */
  void clear() {
    entries_.reset();
    selectorPool_.reset();
    stylePool_.reset();
    entryCount_ = entryCapacity_ = 0;
    selectorPoolSize_ = selectorPoolCapacity_ = 0;
    styleCount_ = styleCapacity_ = 0;
    ruleGrowthStopped_ = false;
    hasIdRules_ = hasCompoundRules_ = hasContextualRules_ = hasFirstLetterRules_ = false;
  }

  /**
   * Check if CSS rules cache file exists
   */
  bool hasCache() const;

  /** Read the cache header without hydrating its rule map. */
  CacheStatus inspectCache() const;

  /**
   * Delete CSS rules cache file exists
   */
  void deleteCache() const;

  /**
   * Save parsed CSS rules to a cache file.
   * @return true if cache was written successfully
   */
  bool saveToCache(bool complete) const;

  /**
   * Load CSS rules from a cache file.
   * Clears any existing rules before loading.
   * @return Complete when loaded, LowMemory when it should be retried, otherwise Invalid
   */
  CacheLoadResult loadFromCache();

 private:
  enum class RuleInsertResult : uint8_t {
    Inserted,
    Merged,
    Limit,
    OutOfMemory,
  };

  enum class PoolResult : uint8_t {
    Ready,
    Limit,
    OutOfMemory,
  };

  // A lookup key assembled from views, so keys are compared without being copied
  // into a buffer. Stored keys are the canonical subject compound (element, #id,
  // sorted classes), optionally followed by CONTEXT_SEPARATOR and the lowercase
  // ancestor part of a descendant/child selector (e.g. "div.poem > ").
  struct KeyPieces {
    static constexpr size_t MAX_PIECES = 14;
    std::string_view piece[MAX_PIECES];
    uint8_t count = 0;
    size_t length = 0;
    bool add(const std::string_view view) {
      if (view.empty()) return true;
      if (count == MAX_PIECES) return false;
      piece[count++] = view;
      length += view.size();
      return true;
    }
  };

  struct SelectorEntry {
    uint32_t offset;
    uint16_t styleIndex;
    uint16_t length;
  };
  static_assert(sizeof(SelectorEntry) == 8);

  // Bounded flat storage keeps every growth operation fallible and avoids the
  // throwing node allocations used by std::unordered_map.
  std::unique_ptr<SelectorEntry[]> entries_;
  std::unique_ptr<char[]> selectorPool_;
  std::unique_ptr<CssStyle[]> stylePool_;
  uint16_t entryCount_ = 0;
  uint16_t entryCapacity_ = 0;
  uint32_t selectorPoolSize_ = 0;
  uint32_t selectorPoolCapacity_ = 0;
  uint16_t styleCount_ = 0;
  uint16_t styleCapacity_ = 0;
  bool ruleGrowthStopped_ = false;
  // Let resolveStyle skip lookups no stored rule can satisfy.
  bool hasIdRules_ = false;
  bool hasCompoundRules_ = false;
  bool hasContextualRules_ = false;
  bool hasFirstLetterRules_ = false;

  std::string cachePath;

  // Internal parsing helpers
  bool restoreCacheBackupIfNeeded() const;
  void processRuleBlockWithStyle(std::string_view selectorGroup, const CssStyle& style);
  [[nodiscard]] int compareEntryToPieces(const SelectorEntry& entry, const KeyPieces& key,
                                         bool prefixOnly = false) const;
  [[nodiscard]] size_t lowerBound(const KeyPieces& key, bool& exact) const;
  [[nodiscard]] std::string_view selectorAt(size_t index) const;
  RuleInsertResult insertOrMerge(const KeyPieces& key, const CssStyle& style);
  void noteRuleShape(std::string_view storedKey);
  PoolResult ensureEntryCapacity(size_t needed);
  PoolResult ensureSelectorPoolCapacity(size_t needed);
  PoolResult ensureStyleCapacity(size_t needed);
  PoolResult internStyle(const CssStyle& style, uint16_t& indexOut);
  static CssStyle parseDeclarations(std::string_view declBlock);
  static void parseDeclarationIntoStyle(std::string_view decl, CssStyle& style);
  static void parseBorderDeclaration(std::string_view name, std::string_view value, CssStyle& style);

  // Individual property value parsers
  static CssTextAlign interpretAlignment(std::string_view val);
  static CssFontStyle interpretFontStyle(std::string_view val);
  static CssFontWeight interpretFontWeight(std::string_view val);
  static CssTextDecoration interpretDecoration(std::string_view val);
  static CssLength interpretLength(std::string_view val);
  static bool tryInterpretFontSize(std::string_view val, CssLength& out);
  /** Returns true only when a numeric length was parsed (e.g. 2em, 50%). False for auto/inherit/initial. */
  static bool tryInterpretLength(std::string_view val, CssLength& out);
};
