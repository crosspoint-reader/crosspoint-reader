#pragma once

#include <NorFlash.h>

#include <cstdint>
#include <functional>
#include <memory>

#include "EpdFont.h"
#include "EpdFontData.h"
#include "FlashGlyphStore.h"
#include "GlyphProvider.h"
#include "TtfFace.h"
#include "TtfFamily.h"
#include "TtfRasterizer.h"

/// One TrueType family drawn from the glyph store (spec 3.2), in flash mode: every style's whole TTF is copied
/// into the `spiffs` partition when the family is installed, and read there in place. A glyph is rendered the
/// first time it is looked up and stored, so later lookups, in this session and the next, read it from flash.
///
/// It serves one face, the body size: four EpdFontData without tables, so every lookup goes to a per-style
/// GlyphProvider adapter. Kerning, ligatures and the size-ladder rungs come in later stages.
///
/// Not thread-safe: every call runs under RenderLock (spec 3.4).
class TtfFont {
 public:
  /// The largest reader size: the scratch block is planned for it, and setBodySize() refuses anything larger.
  static constexpr uint8_t kMaxPointSize = 26;
  static constexpr uint8_t kRingSlots = 8;
  /// Missing glyphs one prefetch() call renders at most; the rest render as they are looked up.
  static constexpr uint8_t kPrefetchGlyphs = 64;

  /// Supplied by the app; both may stay null (host tests).
  struct Hooks {
    bool (*renderLockHeld)() = nullptr;  ///< debug check of spec 3.4, logged once when false
    void (*yield)() = nullptr;           ///< called between glyphs and between install chunks
  };
  static void setHooks(const Hooks& hooks);

  explicit TtfFont(NorFlash& flash);
  TtfFont(const TtfFont&) = delete;
  TtfFont& operator=(const TtfFont&) = delete;

  /// Opens the store for this family. When the partition holds no store, another family's, or one whose
  /// mappings no longer fit, installs it first: calls `onBusy`, then copies every style from `files` (size 0:
  /// the family has no such style). Finishes a pending wipe, behind `onBusy` too. False when the family cannot
  /// be drawn: no regular style, one the engine cannot read, more than the partition holds, or out of memory.
  bool load(const ttf::FileSource (&files)[4], const uint8_t identity[glyphstore::kIdentityBytes],
            const std::function<void()>& onBusy = {});

  /// Serves `pointSize`: the face metrics at that size, and its size index in the store. False when it is 0
  /// or over kMaxPointSize, or its line height does not fit EpdFontData (advanceY over 255).
  bool setBodySize(uint8_t pointSize);
  uint8_t bodySize() const { return pointSize_; }
  /// The body face's font for `style`; nullptr for a style the family lacks (EpdFontFamily then falls back).
  const EpdFont* font(uint8_t style) const;

  /// Runs a pending wipe (the store filled up, or a write failed), calling `onBusy` first. Only where no glyph
  /// record of this font is in use: at book open. True when a wipe ran.
  bool wipeIfPending(const std::function<void()>& onBusy = {});
  /// New glyphs are drawn without being stored until the next wipe.
  bool ringMode() const { return ringMode_; }

  uint32_t renders() const { return renders_; }
  uint32_t storeHits() const { return hits_; }

  // The provider side; the per-style adapters forward here.
  const EpdGlyph* glyph(uint8_t style, uint32_t codepoint);
  bool ownsBitmap(const EpdGlyph* glyph, const uint8_t*& bitmap) const;
  void prefetch(uint8_t style, const char* utf8Text);

 private:
  class StyleProvider final : public GlyphProvider {
   public:
    TtfFont* owner = nullptr;
    uint8_t style = 0;
    const EpdGlyph* missGlyph(const uint32_t codepoint) override { return owner->glyph(style, codepoint); }
    bool ownsBitmap(const EpdGlyph* glyph, const uint8_t*& bitmap) const override {
      return owner->ownsBitmap(glyph, bitmap);
    }
    void prefetch(const char* utf8Text) override { owner->prefetch(style, utf8Text); }
  };

  struct Style {
    bool present = false;
    uint8_t block = 0;  // the store's style block
    uint32_t bytes = 0;
    ttf::Face face;
    EpdFontData data{};
    EpdFont font{&data};
    StyleProvider provider;
  };

  struct Pending {
    uint32_t codepoint;
    uint16_t glyph;
  };

  // What opening the installed styles found: MapFailed means a style's block cannot be mapped (the budget shrank
  // since the install); Failed, no usable regular style or an unreadable header.
  enum class StylesResult : uint8_t { Ok, MapFailed, Failed };

  StylesResult openStyles(const ttf::FileSource (&files)[4], ttf::ScratchPlan* plan);
  bool install(const ttf::FileSource (&files)[4], const uint8_t* identity, const std::function<void()>& onBusy);
  bool fontBytes(uint8_t style, FontBytes* bytes);
  bool storeKey(uint8_t style, uint32_t codepoint, uint32_t* key);
  void claimSize();
  const EpdGlyph* render(uint8_t style, uint32_t codepoint, bool haveKey, uint32_t key);
  const EpdGlyph* drawToRing(const EpdGlyph& metrics);
  void failed(uint8_t style, uint32_t codepoint, ttf::Status status, bool haveKey, uint32_t key);
  void noteCommit(FlashGlyphStore::Commit result);
  uint32_t batchCapacity() const;
  void checkLock() const;

  NorFlash& flash_;
  FlashGlyphStore store_;
  ttf::Rasterizer rasterizer_;
  Style styles_[4];
  uint32_t ppem_ = 0;
  uint8_t pointSize_ = 0;
  uint8_t sizeIndex_ = 0;
  bool sizeValid_ = false;
  uint32_t sizeGeneration_ = 0;
  bool ringMode_ = false;

  // The ring: glyphs drawn without being stored. Their records live until the slot is reused, 8 lookups later.
  EpdGlyph ringGlyphs_[kRingSlots] = {};
  std::unique_ptr<uint8_t[]> ringBitmaps_[kRingSlots];
  uint16_t ringCapacity_[kRingSlots] = {};
  uint8_t ringNext_ = 0;

  Pending pending_[kPrefetchGlyphs] = {};
  uint32_t renders_ = 0;
  uint32_t hits_ = 0;
  uint8_t failuresLogged_ = 0;
};
