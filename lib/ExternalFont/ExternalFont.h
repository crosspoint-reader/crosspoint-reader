#pragma once

#include <SDCardManager.h>
#include <cstdint>

/**
 * External font loader - supports Xteink .bin format
 * Filename format: FontName_size_WxH.bin (e.g. KingHwaOldSong_38_33x39.bin)
 *
 * Font format:
 * - Direct Unicode codepoint indexing
 * - Offset = codepoint * bytesPerChar
 * - Each char = bytesPerRow * charHeight bytes
 * - 1-bit black/white bitmap, MSB first
 */
class ExternalFont {
public:
  ExternalFont() = default;
  ~ExternalFont();

  // Disable copy
  ExternalFont(const ExternalFont &) = delete;
  ExternalFont &operator=(const ExternalFont &) = delete;

  /**
   * Load font from .bin file
   * @param filepath Full path on SD card (e.g.
   * "/fonts/KingHwaOldSong_38_33x39.bin")
   * @return true on success
   */
  bool load(const char *filepath);

  /**
   * Get glyph bitmap data (with LRU cache)
   * @param codepoint Unicode codepoint
   * @return Bitmap data pointer, nullptr if char not found
   */
  const uint8_t *getGlyph(uint32_t codepoint);

  /**
   * Preload multiple glyphs at once (optimized for batch SD reads)
   * Call this before rendering a chapter to warm up the cache
   * @param codepoints Array of unicode codepoints to preload
   * @param count Number of codepoints in the array
   */
  void preloadGlyphs(const uint32_t *codepoints, size_t count);

  // Font properties
  uint8_t getCharWidth() const { return _charWidth; }
  uint8_t getCharHeight() const { return _charHeight; }
  uint8_t getBytesPerRow() const { return _bytesPerRow; }
  uint16_t getBytesPerChar() const { return _bytesPerChar; }
  const char *getFontName() const { return _fontName; }
  uint8_t getFontSize() const { return _fontSize; }

  bool isLoaded() const { return _isLoaded; }
  void unload();

  /**
   * Get cached metrics for a glyph.
   * Must call getGlyph() first to ensure it's loaded!
   * @param cp Unicode codepoint
   * @param outMinX Minimum X offset (left bearing)
   * @param outAdvanceX Advance width for cursor positioning
   * @return true if metrics found in cache, false otherwise
   */
  bool getGlyphMetrics(uint32_t cp, uint8_t *outMinX, uint8_t *outAdvanceX);

private:
  // Font file handle (keep open to avoid repeated open/close)
  FsFile _fontFile;
  bool _isLoaded = false;

  // Properties parsed from filename
  char _fontName[32] = {0};
  uint8_t _fontSize = 0;
  uint8_t _charWidth = 0;
  uint8_t _charHeight = 0;
  uint8_t _bytesPerRow = 0;
  uint16_t _bytesPerChar = 0;

  // LRU cache - 256 glyphs for better Chinese text performance
  // Memory: ~52KB (256 * 204 bytes per entry)
  static constexpr int CACHE_SIZE = 256; // 256 glyphs
  static constexpr int MAX_GLYPH_BYTES =
      200; // Max 200 bytes per glyph (enough for 33x39)

  // Flag to mark cached "non-existent" glyphs (avoid repeated SD reads)
  static constexpr uint8_t GLYPH_NOT_FOUND_MARKER = 0xFE;

  struct CacheEntry {
    uint32_t codepoint = 0xFFFFFFFF; // Invalid marker
    uint8_t bitmap[MAX_GLYPH_BYTES];
    uint32_t lastUsed = 0;
    bool notFound = false; // True if glyph doesn't exist in font
    uint8_t minX = 0;      // Cached rendering metrics
    uint8_t advanceX = 0;  // Cached advance width
  };
  CacheEntry _cache[CACHE_SIZE];
  uint32_t _accessCounter = 0;

  // Simple hash table for O(1) cache lookup (codepoint -> cache index, -1 if
  // not cached)
  int16_t _hashTable[CACHE_SIZE];
  static int hashCodepoint(uint32_t cp) { return cp % CACHE_SIZE; }

  /**
   * Read glyph data from SD card
   */
  bool readGlyphFromSD(uint32_t codepoint, uint8_t *buffer);

  /**
   * Parse filename to get font parameters
   * Format: FontName_size_WxH.bin
   */
  bool parseFilename(const char *filename);

  /**
   * Find glyph in cache
   * @return Cache index, -1 if not found
   */
  int findInCache(uint32_t codepoint);

  /**
   * Get LRU cache slot (least recently used)
   */
  int getLruSlot();
};
