#pragma once

#include <BuildArena.h>
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
#include "TtfOpenType.h"
#include "TtfRasterizer.h"

class TtfFontSet;

/// One TrueType family drawn from the glyph store (spec 3.2). In flash mode every style's whole TTF is copied into
/// the `spiffs` partition when the family is installed, and read there in place; in SD mode only its tables are,
/// and the outlines are read from the file on the card (spec 5.1). A glyph is rendered the first time it is looked
/// up and stored, so later lookups, in this session and the next, read it from flash in either mode. It belongs to a
/// TtfFontSet, the families installed together, whose store and scratch block it shares.
///
/// It serves the body size and, on demand, native sizes (headings, CSS sizes, drop caps at their own pixel size):
/// each size is an EpdFontData without tables for each style the family has, so every lookup goes to a per-style
/// GlyphProvider adapter, and has its own entry in the store's face table. Kerning comes from the font's GPOS (or
/// legacy kern) table through the store's pair index, and the fi/fl/ff/ffi/ffl ligatures from its GSUB table
/// (spec 5.4).
///
/// Not thread-safe: every call runs under RenderLock (spec 3.4).
class TtfFont {
 public:
  /// The largest reader size: the scratch block is planned for it, and setBodySize() refuses anything larger.
  static constexpr uint8_t kMaxPointSize = 26;
  /// Native sizes, in whole pixels per em. The scratch block is planned for the largest: its output region then
  /// holds any glyph up to 4 KB, and a glyph's top stays inside EpdGlyphRef's int8. Larger text is drawn from the
  /// largest size scaled.
  static constexpr uint8_t kMinNativePx = 10;
  static constexpr uint8_t kMaxNativePx = 120;
  /// Native sizes held at once, until dropNativeSizes().
  static constexpr uint8_t kMaxNativeSizes = 16;

  /// Supplied by the app; all may stay null (host tests).
  struct Hooks {
    bool (*renderLockHeld)() = nullptr;  ///< debug check of spec 3.4, logged once when false
    void (*yield)() = nullptr;           ///< called between glyphs and between install chunks
    /// Frees what the app can spare when a miss's memory does not fit the heap (the built-in fonts' page caches),
    /// before the miss asks once more. True when it freed something.
    bool (*reclaim)() = nullptr;
    /// Milliseconds since boot, to time a wipe (TtfFontSet::storeEvents()); without it wipes are counted untimed.
    uint32_t (*nowMs)() = nullptr;
  };
  static void setHooks(const Hooks& hooks);

  ~TtfFont();
  TtfFont(const TtfFont&) = delete;
  TtfFont& operator=(const TtfFont&) = delete;

  /// Serves `pointSize`: the face metrics at that size, and its face index in the store. False when it is 0
  /// or over kMaxPointSize, or its line height does not fit EpdFontData (advanceY over 255).
  bool setBodySize(uint8_t pointSize);
  uint8_t bodySize() const { return pointSize_; }
  /// The body face's font for `style`; nullptr for a style the family lacks (EpdFontFamily then falls back).
  const EpdFont* font(uint8_t style) const;
  /// The body face's pixels per em (26.6), which native sizes are measured against; 0 before setBodySize().
  uint32_t bodyPpem() const { return body_.ppem; }
  /// The family's largest native size: kMaxNativePx, or less when its tallest glyph (head.yMax) or the furthest
  /// left it reaches (head.xMin) would not fit EpdGlyphRef's int8 there. Larger text is this size scaled.
  uint8_t maxNativePx() const { return maxNativePx_; }

  /// The fonts of the native size `px` (whole pixels per em), by style, made on first use; nullptr for a style the
  /// family lacks. When all kMaxNativeSizes are held, or memory is short, the nearest size held serves instead:
  /// `servedPpem` (26.6) says which, so the caller can scale it to `px`. False, with `fonts` untouched, when `px`
  /// is outside kMinNativePx..kMaxNativePx, a line height does not fit EpdFontData, or no size can serve. They live
  /// until dropNativeSizes() or the font's end.
  bool nativeFonts(uint8_t px, const EpdFont* (&fonts)[4], uint32_t* servedPpem = nullptr);
  /// Frees every native size, and the set's ring with them. Only once nothing refers to their fonts any more
  /// (their font IDs removed) and no glyph of the set is in use.
  void dropNativeSizes();
  uint8_t nativeSizeCount() const;

  /// New glyphs are drawn without being stored until the next wipe: the set's store filled up, or failed.
  bool ringMode() const;
  /// A style of the family reads its outlines from its file on the card (SD mode): that file stays open while the
  /// set is loaded.
  bool readsCard() const;

  uint32_t renders() const { return renders_; }
  uint32_t storeHits() const { return hits_; }
  /// Pairs whose kerning was worked out from the font, not read from the store.
  uint32_t kernLookups() const { return kernLookups_; }

  /// The kerning between two codepoints of `style` at the body size, in 4.4 pixels (EpdFont's unit).
  int8_t kerning(uint8_t style, uint32_t left, uint32_t right) { return kerning(body_, style, left, right); }

  // The body size's glyphs, as its per-style adapters see them.
  const EpdGlyph* glyph(uint8_t style, uint32_t codepoint) { return glyph(body_, style, codepoint); }
  bool ownsBitmap(const EpdGlyph* glyph, const uint8_t*& bitmap) const;
  void prefetch(uint8_t style, const char* utf8Text) { prefetch(body_, style, utf8Text); }

 private:
  friend class TtfFontSet;
  struct Size;

  class StyleProvider final : public GlyphProvider {
   public:
    TtfFont* owner = nullptr;
    Size* size = nullptr;
    uint8_t style = 0;
    const EpdGlyph* missGlyph(const uint32_t codepoint) override { return owner->glyph(*size, style, codepoint); }
    bool ownsBitmap(const EpdGlyph* glyph, const uint8_t*& bitmap) const override {
      return owner->ownsBitmap(glyph, bitmap);
    }
    void prefetch(const char* utf8Text) override { owner->prefetch(*size, style, utf8Text); }
    int8_t kerning(const uint32_t leftCp, const uint32_t rightCp) override {
      return owner->kerning(*size, style, leftCp, rightCp);
    }
  };

  /// One pixel size: its index in the store's face table and the fonts of the family's styles. Never copied or moved:
  /// each font points at its own data, and each provider at its size.
  struct Size {
    struct StyleFont {
      EpdFontData data{};
      EpdFont font{&data};
      StyleProvider provider;
    };
    Size() = default;
    Size(const Size&) = delete;
    Size& operator=(const Size&) = delete;

    uint32_t ppem = 0;  // 26.6; 0 until configured
    uint8_t index = 0;
    bool indexValid = false;
    bool claimTried = false;  // in store generation `generation`
    uint32_t generation = 0;
    // One for each style the family has a file for, where slot_ says: the body's made at load, a native size's with it.
    std::unique_ptr<StyleFont[]> styles;
  };

  // A style's file, shared by every size.
  struct Style {
    bool present = false;
    bool onCard = false;  // SD mode: the block holds the tables, `sd` the outlines on the card
    uint8_t block = 0;    // the store's style block
    uint32_t bytes = 0;
    ttf::SdRange sd;
    ttf::Face face;
    ttf::Kerning kerning;
    ttf::Ligatures ligatures;  // every size's EpdFontData points at its pairs
    // Left codepoints found to kern with nothing. Their pairs are never stored (spec 8.1), yet the space before
    // every word asks for one: one bit each below kUncoveredEnd (Latin), so they read the font once per load.
    static constexpr uint32_t kUncoveredEnd = 0x250;
    uint8_t uncoveredLeft[kUncoveredEnd / 8] = {};

    bool knownUncovered(const uint32_t left) const {
      return left < kUncoveredEnd && (uncoveredLeft[left >> 3] >> (left & 7) & 1) != 0;
    }
    void markUncovered(const uint32_t left) {
      if (left < kUncoveredEnd) uncoveredLeft[left >> 3] |= static_cast<uint8_t>(1u << (left & 7));
    }
  };

  // What opening the installed styles found: MapFailed means a style's block cannot be mapped (the budget shrank
  // since the install); Changed, an SD-mode style's file on the card is not the one installed; Failed, no usable
  // regular style or an unreadable header.
  enum class StylesResult : uint8_t { Ok, MapFailed, Changed, Failed };

  TtfFont(TtfFontSet& set, uint8_t family) : set_(set), family_(family) {}
  // The style `style`, when the family has a file for it; else nullptr.
  Style* styleAt(const uint8_t style) const {
    return style < 4 && slot_[style] != kNoSlot ? &styles_[slot_[style]] : nullptr;
  }
  // Whether the family draws `style`: it has a file for it that the engine reads.
  bool draws(const uint8_t style) const {
    const Style* const found = styleAt(style);
    return found && found->present;
  }
  StylesResult openStyles(const ttf::FileSource (&files)[4], uint8_t& block, ttf::ScratchPlan* plan);
  bool fontBytes(uint8_t style, FontBytes* bytes);
  bool configure(Size& size, uint32_t ppem);
  void rebase(Size& size);
  Size* nativeSize(uint8_t px);
  Size* nearestNative(uint32_t ppem);
  bool claimSize(Size& size);
  bool storeKey(Size& size, uint8_t style, uint32_t codepoint, uint32_t* key);
  const EpdGlyph* glyph(Size& size, uint8_t style, uint32_t codepoint);
  int8_t kerning(const Size& size, uint8_t style, uint32_t left, uint32_t right);
  void prefetch(Size& size, uint8_t style, const char* utf8Text);
  const EpdGlyph* render(const Size& size, uint8_t style, uint32_t codepoint, bool haveKey, uint32_t key);

  TtfFontSet& set_;
  uint8_t family_;  // its index in the set: in its face entries and pair slots
  // Only the styles the family has a file for, a book's accent family often one or two (each style ~260 B, and its
  // font in every size ~130 B): slot_ gives a style's place in them and in each size's fonts. Made at load.
  static constexpr uint8_t kNoSlot = 0xFF;
  std::unique_ptr<Style[]> styles_;
  uint8_t slot_[4] = {kNoSlot, kNoSlot, kNoSlot, kNoSlot};
  uint8_t styleCount_ = 0;
  Size body_;
  uint8_t pointSize_ = 0;
  std::unique_ptr<Size> native_[kMaxNativeSizes];
  uint8_t maxNativePx_ = kMaxNativePx;
  uint32_t renders_ = 0;
  uint32_t hits_ = 0;
  uint32_t kernLookups_ = 0;
};

/// The TrueType families drawn together from one glyph store (spec 11.1): the reader's body family, and a book's
/// accent families beside it. Their styles are installed together, the set's identity (ttf::setIdentity) telling the
/// store which set it holds, and their glyphs share its log, index and face table. Only one renders at a time
/// (RenderLock), so they share one scratch block, sized for the largest of their styles, one ring and one prefetch
/// list: an accent family costs its faces and tables, never a second block.
///
/// What only a miss needs (spec 5.3), the scratch block and SD mode's page cache, is made for the first glyph drawn
/// that the store does not hold, never at load: a page drawn from the store needs none of it. A lookup takes it from
/// the region lent to the misses for that lookup alone (glyph_miss::Scope, a section build's arena) when it has room,
/// else from the heap, where it stays until releaseMissMemory(). One that cannot be had is counted
/// (glyph_miss::starvedCount) and drawn as missing.
///
/// Not thread-safe: every call runs under RenderLock (spec 3.4).
class TtfFontSet {
 public:
  /// The body's family and three accent families (Publisher Fonts): the store's eight style blocks hold a body of
  /// four styles and three one-style families, and its face table (32 faces) the body's sizes and a few of each.
  static constexpr uint8_t kMaxFamilies = 4;
  static constexpr uint8_t kRingSlots = 8;
  /// The ring's bitmaps together, at most: a native size's glyphs are large.
  static constexpr uint32_t kRingBudgetBytes = 8192;
  /// Missing glyphs one prefetch() call renders at most; the rest render as they are looked up.
  static constexpr uint8_t kPrefetchGlyphs = 64;

  /// One family's files by style (size 0: the family has no such style).
  using Files = ttf::FileSource[4];
  /// Whether load() may install the set. Never opens only a store that holds the set already, as a set loaded again
  /// after the reader's memory ladder freed it does: no erase, no copy and no pending wipe, nothing in the way of
  /// the page it is drawing.
  enum class Install : uint8_t { IfNeeded, Never };

  explicit TtfFontSet(NorFlash& flash);
  TtfFontSet(const TtfFontSet&) = delete;
  TtfFontSet& operator=(const TtfFontSet&) = delete;

  /// Opens the store for these families, in this order, under the set's `identity`. When the partition holds no
  /// store, another set's, one whose mappings no longer fit, or one whose SD-mode file changed, installs it first:
  /// calls `onBusy`, then copies every family's styles. Flash mode while the whole set fits; past that the families
  /// go to SD mode from the last (the least used accent) on, the body last of all. Finishes a pending wipe, behind
  /// `onBusy` too. False when the set cannot be drawn: a family without a regular style or with one the engine
  /// cannot read, more than the partition holds in SD mode, or out of memory; with Install::Never, also a set the
  /// store does not hold as it is. An SD-mode style reads its outlines through its FileSource for as long as the set
  /// is loaded: those files must stay open until then.
  bool load(const Files* families, uint8_t count, const uint8_t identity[glyphstore::kIdentityBytes],
            const std::function<void()>& onBusy = {}, Install install = Install::IfNeeded);
  uint8_t familyCount() const { return count_; }
  /// The set's family `family`, in load()'s order; nullptr past familyCount().
  TtfFont* font(uint8_t family) const { return family < count_ ? fonts_[family].get() : nullptr; }

  /// Runs a pending wipe (the store filled up, or a write failed), calling `onBusy` first. Only where no glyph
  /// record of the set is in use: at book open. True when a wipe ran.
  bool wipeIfPending(const std::function<void()>& onBusy = {});
  /// The store reacts as if it had just filled up: new glyphs go to the ring, and a wipe is pending. The serial test
  /// API's CMD:TTF:FULL, to reach what a full store does at any size. False when no store is open.
  bool markFull();

  /// What the set and its store hold now, for the serial test API's CMD:TTF.
  struct Stats {
    uint8_t families = 0;
    bool readsCard = false;  ///< a family reads its outlines from the card (SD mode)
    bool ringMode = false;
    bool wipePending = false;
    uint32_t logUsed = 0;
    uint32_t logSize = 0;
    uint32_t usedSlots = 0;  ///< glyph index slots in use
    uint32_t renders = 0;    ///< glyphs drawn from outlines since load, every family's
    uint32_t hits = 0;       ///< lookups the store answered since load
  };
  Stats stats() const;

  /// How often a store filled up and was wiped since boot, every set's, and how long the last wipe took (0 without
  /// the nowMs hook). Read from any task: the serial test API polls it for its EVT: lines.
  struct StoreEvents {
    uint32_t fills = 0;
    uint32_t wipes = 0;
    uint32_t lastWipeMs = 0;
  };
  static StoreEvents storeEvents();
  /// New glyphs are drawn without being stored until the next wipe.
  bool ringMode() const { return ringMode_; }
  /// The heap the ring's bitmaps hold.
  uint32_t ringBytes() const;
  /// The one scratch block every family renders in, made or not.
  uint32_t scratchBytes() const { return rasterizer_.scratchBytes(); }
  /// SD mode: the 512-byte pages read from the card since load(), every family's.
  uint32_t sdPageReads() const { return sdPageReads_ + (sdPages_ ? sdPages_->misses() : 0); }
  /// A family reads its outlines from the card (SD mode).
  bool readsCard() const { return readsCard_; }

  /// Frees what the misses made on the heap: the scratch block (its plan kept), SD mode's page cache and the ring.
  /// Glyphs in the store need none of it, and the next miss makes it again. Only where no glyph record of the set is
  /// in use: as RenderLock is given back, and for the reader's memory ladder before an image decode. The bytes freed.
  uint32_t releaseMissMemory();
  /// What releaseMissMemory() would free now.
  uint32_t missMemoryBytes() const;

 private:
  friend class TtfFont;
  class MissScope;

  struct Pending {
    uint32_t codepoint;
    uint16_t glyph;
  };

  TtfFont::StylesResult openStyles(const Files* families, uint8_t count, ttf::ScratchPlan* plan);
  bool install(const Files* families, uint8_t count, const uint8_t* identity, const std::function<void()>& onBusy);
  const EpdGlyph* drawToRing(const EpdGlyph& metrics);
  void freeRingSlot(uint8_t slot);
  void freeRing();
  bool ownsBitmap(const EpdGlyph* glyph, const uint8_t*& bitmap) const;
  void failed(uint8_t style, uint32_t codepoint, ttf::Status status, bool haveKey, uint32_t key);
  void noteCommit(FlashGlyphStore::Commit result);
  uint32_t batchCapacity() const { return rasterizer_.plan().outputBytes; }
  bool beginMiss();
  void endMiss();
  bool heapPages();
  void usePages(ttf::SdPageCache* pages);
  void noMemory(uint32_t codepoint);

  NorFlash& flash_;
  FlashGlyphStore store_;
  ttf::Rasterizer rasterizer_;
  // SD mode's page cache while a miss has one: ownedPages_, or a lookup's block of the lent arena.
  ttf::SdPageCache* sdPages_ = nullptr;
  std::unique_ptr<ttf::SdPageCache> ownedPages_;
  uint32_t sdPageReads_ = 0;        // the pages caches given back since load() read
  BuildArena* lentFrom_ = nullptr;  // the arena the lookup in progress draws in, if any
  BuildArena::Block lent_;
  std::unique_ptr<TtfFont> fonts_[kMaxFamilies];
  uint8_t count_ = 0;
  bool readsCard_ = false;
  bool missesStarved_ = false;  // the heap could not hold a miss's memory in this pass (beginMiss())
  bool ringMode_ = false;

  // The ring: glyphs drawn without being stored. Their records live until the slot is reused, 8 lookups later.
  EpdGlyph ringGlyphs_[kRingSlots] = {};
  std::unique_ptr<uint8_t[]> ringBitmaps_[kRingSlots];
  uint16_t ringCapacity_[kRingSlots] = {};
  uint8_t ringNext_ = 0;

  Pending pending_[kPrefetchGlyphs] = {};
  uint8_t failuresLogged_ = 0;
};
