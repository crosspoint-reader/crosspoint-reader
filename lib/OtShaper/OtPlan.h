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
enum class Script : uint8_t {
  Devanagari,
  Bengali,
  Gurmukhi,
  Gujarati,
  Oriya,
  Tamil,
  Telugu,
  Kannada,
  Malayalam,
  Sinhala
};

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

class PlanBuilder;

// Features whose mask bit the shapers set on glyphs themselves, in the order
// compiled plans store their masks (docs/file-formats.md, CPpl).
enum MaskedFeature {
  MF_RPHF,
  MF_PREF,
  MF_BLWF,
  MF_ABVF,
  MF_HALF,
  MF_PSTF,
  MF_INIT,
  MF_ISOL,
  MF_MEDI,
  MF_FINA,
  MF_COUNT
};
constexpr uint32_t MASKED_FEATURE_TAGS[MF_COUNT] = {tag("rphf"), tag("pref"), tag("blwf"), tag("abvf"), tag("half"),
                                                    tag("pstf"), tag("init"), tag("isol"), tag("medi"), tag("fina")};

class Plan {
 public:
  // Plans `script` from the font's GSUB and GPOS (OtPlanBuild.cpp).
  // `languageTags` are the OpenType language systems to try, in order
  // (zero-terminated, up to three). False when the font has more lookups
  // than a plan takes or the heap check refuses the plan's memory; a plan
  // that failed to build or load is empty, and shapes nothing.
  bool build(const Face& face, Script script, const uint32_t* languageTags);
  // The same plan, read from the font's CPpl table, where the .cpfont
  // converter stored it. False when the font has none for this script and
  // language, or it is malformed.
  bool load(const Face& face, Script script, const uint32_t* languageTags);

  // Lookup settings (flags, mask, filters) of planned lookup `i` of `table`.
  LookupSettings settings(int table, size_t i) const;
  // Heap the plan holds, for memory statistics.
  size_t memoryBytes() const;

  Script script = Script::Devanagari;
  ShaperKind shaper = ShaperKind::Default;
  uint32_t chosenScript = 0;  // the GSUB script tag the plan follows
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
  static constexpr uint8_t NO_STAGE = 0xFF;
  // GSUB stage of each would-substitute feature; NO_STAGE when the font lacks it.
  uint8_t wouldStage[WS_COUNT] = {NO_STAGE, NO_STAGE, NO_STAGE, NO_STAGE, NO_STAGE};
  // Whether the lookups of the stage of `feature` would substitute `glyphs`.
  bool wouldSubstitute(const Face& face, WouldFeature feature, const uint32_t* glyphs, unsigned count) const;

  // Universal Shaping Engine.
  uint32_t useRphfMask = 0;
  uint32_t useTopographicalMasks[4] = {};  // isol, init, medi, fina

 private:
  friend class PlanBuilder;

  // The planned GSUB lookups [start, end) of the stage a feature is applied in.
  struct WouldSubstituteLookups {
    uint16_t start = 0;
    uint16_t end = 0;
  };

  // load() until the plan is complete; false when anything is missing or malformed.
  bool read(const Face& face, Script script, const uint32_t* languageTags);
  // `masks` in MaskedFeature order, as the builder computes them.
  void setFeatureMasks(const uint32_t* masks);
  // Everything that follows from the lookups, stages and masks: GPOS and
  // mark handling, lookup filters and the shapers' data.
  void finish(const Face& face);
  void buildFilters(const Face& face);
  void collectWould(WouldFeature feature);

  bool zeroContext_ = false;
  WouldSubstituteLookups would_[WS_COUNT];
  // Lookup filters: the font's CPac records, or computed here (per planned
  // lookup its digest and where its subtable digests start).
  const FilterRecords* fontFilters_[2] = {nullptr, nullptr};
  std::vector<uint8_t> lookupDigests_[2];
  std::vector<uint32_t> subtableStarts_[2];
  std::vector<uint8_t> subtableDigests_[2];
};

// The script and language system a plan follows in one layout table
// (hb_ot_layout_table_select_script, hb_ot_layout_script_select_language).
struct LanguageSystem {
  uint32_t chosenScript = 0;
  Table langSys;
  // Identifies the choice within the font: the matched LangSys tag, 'dflt',
  // or 0 for the script's default LangSys. Compiled plans are keyed by it.
  uint32_t key = 0;
};
LanguageSystem selectLanguageSystem(const Table& layout, Script script, const uint32_t* languageTags);

}  // namespace ot
