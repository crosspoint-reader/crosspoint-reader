#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "OtBuffer.h"
#include "OtCategories.h"
#include "OtFace.h"
#include "OtLayout.h"

// How one font shapes one script in one language: which lookups run, in
// which stages, with which masks, and which shaper reorders between them.
// Mirrors HarfBuzz's hb_ot_map_builder_t / hb_ot_shape_plan_t.
namespace ot {

enum class ShaperKind : uint8_t { Default, Indic, Use };

// Work done between GSUB stages (HarfBuzz pause callbacks).
enum class Pause : uint8_t {
  None,
  IndicSetupSyllables,
  IndicInitialReordering,
  IndicFinalReordering,
  UseSetupSyllables,
  UseClearSubstitutionFlags,
  UseRecordRphf,
  UseRecordPref,
  UseReorder,
};

// Indic scripts shaped here, in Unicode block order from U+0900.
enum class Script : uint8_t { Devanagari, Bengali, Gurmukhi, Gujarati, Oriya, Tamil, Telugu, Kannada, Malayalam, Sinhala };

// Most lookups a plan takes per table; fonts with more are not shaped.
constexpr size_t MAX_PLANNED_LOOKUPS = 4096;

struct PlannedLookup {
  uint16_t index;
  uint8_t flags;  // lookupbits::*
  uint32_t mask;
};

namespace lookupbits {
constexpr uint8_t AUTO_ZWNJ = 0x01;     // skip ZWNJ when matching context
constexpr uint8_t AUTO_ZWJ = 0x02;      // skip ZWJ when matching input
constexpr uint8_t RANDOM = 0x04;        // 'rand': pick alternates at random
constexpr uint8_t PER_SYLLABLE = 0x08;  // match within one syllable
}  // namespace lookupbits

struct Stage {
  uint16_t lastLookup;  // lookups before this index belong to this or earlier stages
  Pause pause;          // run after the stage's lookups
};

class FeatureBuilder;

class Plan {
 public:
  // `languageTags` are the OpenType language systems to try, in order
  // (zero-terminated, up to three). False when the font has more lookups
  // than a plan takes or the heap check refuses the plan's memory.
  bool build(const Face& face, Script script, const uint32_t* languageTags);

  // Lookup settings (flags, mask, filters) of planned lookup `i` of `table`.
  LookupSettings settings(int table, size_t i) const;
  // Bit value of `tag` in glyph masks; 0 when the font lacks the feature.
  uint32_t oneMask(uint32_t tag) const;
  // Heap the plan holds, for memory statistics.
  size_t memoryBytes() const;

  Script script = Script::Devanagari;
  ShaperKind shaper = ShaperKind::Default;
  uint32_t globalMask = 0;
  std::vector<PlannedLookup> lookups[2];
  std::vector<Stage> stages[2];
  bool applyGpos = false;
  bool fallbackGlyphClasses = false;  // no GDEF classes: guess them from Unicode
  bool zeroMarks = false;             // shaper zeroes mark advances
  bool adjustMarkPositioningWhenZeroing = false;

  // Indic shaper.
  const IndicConfig* indicConfig = nullptr;
  bool isOldSpec = false;  // the font uses v1 script tags (deva, beng, ...)
  enum IndicMask { RPHF, PREF, BLWF, ABVF, HALF, PSTF, INIT, INDIC_MASK_COUNT };
  uint32_t indicMasks[INDIC_MASK_COUNT] = {};
  enum WouldFeature { WS_RPHF, WS_PREF, WS_BLWF, WS_PSTF, WS_VATU, WS_COUNT };
  // Whether the lookups of the stage of `feature` would substitute `glyphs`.
  bool wouldSubstitute(const Face& face, WouldFeature feature, const uint32_t* glyphs, unsigned count) const;

  // Universal Shaping Engine.
  uint32_t useRphfMask = 0;
  uint32_t useTopographicalMasks[4] = {};  // isol, init, medi, fina

 private:
  struct FeatureMap {
    uint32_t tag;
    unsigned index[2];  // feature index in GSUB and GPOS, or 0xFFFF
    uint8_t stage[2];
    uint8_t lookupFlags;
    uint8_t shift;
    uint32_t mask;
  };
  struct LanguageSystem {
    uint32_t chosenScript = 0;
    Table langSys;
    unsigned requiredIndex = 0xFFFF;
    uint32_t requiredTag = 0;
  };
  struct WouldSubstituteLookups {
    std::vector<uint16_t> lookups;
    std::vector<Digest> digests;
  };

  void selectLanguageSystems(const Face& face, const uint32_t* languageTags, LanguageSystem* systems) const;
  void chooseShaper(uint32_t chosenScript);
  void mapFeatures(const Face& face, const LanguageSystem* systems, const FeatureBuilder& builder,
                   unsigned* requiredStage);
  bool planLookups(const Face& face, const LanguageSystem* systems, const FeatureBuilder& builder,
                   const unsigned* requiredStage);
  void buildFilters(const Face& face);
  void setupShaperData(const Face& face, uint32_t chosenScript);
  void collectWould(const Face& face, WouldFeature feature, uint32_t tag);

  std::vector<FeatureMap> features_;
  bool zeroContext_ = false;
  WouldSubstituteLookups would_[WS_COUNT];
  // Lookup filters: the font's CPac records, or computed here (per planned
  // lookup its digest and where its subtable digests start).
  const FilterRecords* fontFilters_[2] = {nullptr, nullptr};
  std::vector<uint8_t> lookupDigests_[2];
  std::vector<uint32_t> subtableStarts_[2];
  std::vector<uint8_t> subtableDigests_[2];
};

}  // namespace ot
