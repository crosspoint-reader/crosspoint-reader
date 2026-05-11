#pragma once

#include <HalStorage.h>

#include <list>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

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
 */
class CssParser {
 public:
  struct ResolveStats {
    uint32_t resolveCalls = 0;
    uint32_t lowHeapSkips = 0;
    uint32_t mapHits = 0;
    uint32_t hotHits = 0;
    uint32_t diskHits = 0;
    uint32_t misses = 0;
    uint32_t negativeHits = 0;
  };

  // Bump when CSS cache format or rules change; section caches are invalidated when this changes
  static constexpr uint8_t CSS_CACHE_VERSION = 4;

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
   * Look up the style for an HTML element, considering tag name and class attributes.
   * Applies CSS cascade: element style < class style < element.class style
   *
   * @param tagName The HTML element name (e.g., "p", "div")
   * @param classAttr The class attribute value (may contain multiple space-separated classes)
   * @return Combined style with all applicable rules merged
   */
  [[nodiscard]] CssStyle resolveStyle(const std::string& tagName, const std::string& classAttr) const;

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

  // CSS lookup telemetry helpers for tuning memory/caching behavior on-device.
  void resetResolveStats() const;
  [[nodiscard]] ResolveStats getResolveStats() const;
  void logResolveStats(const char* context) const;

 private:
  // Storage: maps normalized selector -> style properties
  std::unordered_map<std::string, CssStyle> rulesBySelector_;

  std::string cachePath;

  // Disk-backed CSS dictionary index: selector -> byte offset for serialized CssStyle payload.
  // Built from cache file once, then styles are loaded on demand into hotRuleCache_.
  mutable bool cacheIndexLoaded_ = false;
  mutable size_t cachedRuleCount_ = 0;
  mutable std::unordered_map<std::string, uint32_t> cacheRuleOffsets_;

  // Bounded hot cache of most recently used rules.
  mutable std::list<std::string> hotRuleLru_;
  mutable std::unordered_map<std::string, std::pair<CssStyle, std::list<std::string>::iterator>> hotRuleCache_;
  mutable std::unordered_set<std::string> negativeRuleCache_;
  mutable ResolveStats resolveStats_;

  bool compileModeActive_ = false;
  bool compileModeFailed_ = false;
  std::string compileTempPath_;
  FsFile compileTempFile_;
  std::unordered_map<std::string, uint32_t> compileSelectorOffsets_;

  // Internal parsing helpers
  void processRuleBlockWithStyle(const std::string& selectorGroup, const CssStyle& style);
  static CssStyle parseDeclarations(const std::string& declBlock);
  static void parseDeclarationIntoStyle(const std::string& decl, CssStyle& style, std::string& propNameBuf,
                                        std::string& propValueBuf);

  // Individual property value parsers
  static CssTextAlign interpretAlignment(const std::string& val);
  static CssFontStyle interpretFontStyle(const std::string& val);
  static CssFontWeight interpretFontWeight(const std::string& val);
  static CssTextDecoration interpretDecoration(const std::string& val);
  static CssLength interpretLength(const std::string& val);
  /** Returns true only when a numeric length was parsed (e.g. 2em, 50%). False for auto/inherit/initial. */
  static bool tryInterpretLength(const std::string& val, CssLength& out);

  // String utilities
  static std::string normalized(const std::string& s);
  static void normalizedInto(const std::string& s, std::string& out);
  static std::vector<std::string> splitOnChar(const std::string& s, char delimiter);
  static std::vector<std::string> splitWhitespace(const std::string& s);

  // On-demand rule loading helpers
  bool ensureCacheIndexLoaded() const;
  bool lookupRule(const std::string& selector, CssStyle& outStyle) const;
  bool readRuleFromDiskAtOffset(uint32_t styleOffset, CssStyle& outStyle) const;
  static bool readCssStylePayload(FsFile& file, CssStyle& style);
  static void writeCssStylePayload(FsFile& file, const CssStyle& style);
  void touchHotRule(const std::string& selector) const;
  void cacheHotRule(const std::string& selector, const CssStyle& style) const;
};
