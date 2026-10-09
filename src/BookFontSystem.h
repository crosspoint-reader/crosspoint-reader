#pragma once

#include <VectorFontSupport.h>

#if CROSSPOINT_VECTOR_FONTS

#include <Epub/blocks/TextBlock.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class Epub;
class GfxRenderer;
class TtfEpdFont;

/// Fonts embedded in the open EPUB through @font-face. Each family's files are read
/// from the book into PSRAM the first time layout asks for that family, then rendered
/// at any size through TtfEpdFont. PSRAM boards only: a family can hold a few hundred
/// KB of font bytes plus per-size glyph caches.
class BookFontSystem {
 public:
  BookFontSystem();
  ~BookFontSystem();
  BookFontSystem(const BookFontSystem&) = delete;
  BookFontSystem& operator=(const BookFontSystem&) = delete;

  /// Read the book's @font-face table. Safe to call again for the same book.
  void open(const std::shared_ptr<Epub>& epub, GfxRenderer& renderer);
  /// Unregister every font and free the font bytes.
  void close();

  /// Font id for `family` at `scale` times the reader size, or 0 when the book does not embed it.
  int resolve(uint32_t family, float scale);
  /// Re-register a font id resolve() handed out earlier (after eviction or a reboot).
  bool load(int fontId);

 private:
  // A font file read from the book, in PSRAM.
  struct FontBytes {
    uint8_t* data = nullptr;
    size_t size = 0;
    ~FontBytes();
  };
  struct Family {
    uint32_t hash = 0;
    std::string hrefs[4];  // by TtfEpdFont::Style role; empty = absent
    std::unique_ptr<FontBytes> bytes[4];
    bool bytesLoaded = false;  // read attempted
  };
  struct Loaded {
    int fontId;
    uint32_t lastUse;
    std::unique_ptr<TtfEpdFont> font;
  };
  // Block font, word slots, and a drop cap.
  static constexpr size_t MAX_LOADED = TextBlock::MAX_WORD_FONTS + 2;

  static int fontIdFor(uint32_t family, uint8_t pointSize);
  bool readFamilyBytes(Family& family);
  bool loadFont(Family& family, uint8_t pointSize, int fontId);
  void unloadFonts();

  std::shared_ptr<Epub> epub_;
  GfxRenderer* renderer_ = nullptr;
  std::vector<Family> families_;
  std::vector<Loaded> loaded_;
  uint32_t clock_ = 0;
};

extern BookFontSystem bookFontSystem;

#endif  // CROSSPOINT_VECTOR_FONTS
