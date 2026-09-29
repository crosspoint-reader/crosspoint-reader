#include "OtPlan.h"

#include <algorithm>

namespace ot {

namespace {

constexpr unsigned NOT_FOUND = 0xFFFF;
constexpr unsigned GLOBAL_BIT_SHIFT = 31;
constexpr uint32_t GLOBAL_BIT_MASK = 1u << GLOBAL_BIT_SHIFT;
constexpr unsigned MAX_BITS = 8;  // HB_OT_MAP_MAX_BITS
// Feature bits start where HarfBuzz's do (it keeps three glyph flags below
// them), so fonts run out of mask bits exactly as they would in HarfBuzz.
constexpr unsigned FIRST_FEATURE_BIT = 4;

// Feature flags (hb_ot_map_feature_flags_t).
namespace ff {
constexpr uint8_t GLOBAL = 0x01;
constexpr uint8_t HAS_FALLBACK = 0x02;
constexpr uint8_t MANUAL_ZWNJ = 0x04;
constexpr uint8_t MANUAL_ZWJ = 0x08;
constexpr uint8_t MANUAL_JOINERS = MANUAL_ZWNJ | MANUAL_ZWJ;
constexpr uint8_t GLOBAL_MANUAL_JOINERS = GLOBAL | MANUAL_JOINERS;
constexpr uint8_t GLOBAL_HAS_FALLBACK = GLOBAL | HAS_FALLBACK;
constexpr uint8_t RANDOM = 0x20;
constexpr uint8_t PER_SYLLABLE = 0x40;
}  // namespace ff

// OpenType script tags tried in order: Indic v3 (shaped by USE), v2, v1.
// Sinhala has one.
constexpr uint32_t SCRIPT_TAGS[][3] = {
    {tag("dev3"), tag("dev2"), tag("deva")}, {tag("bng3"), tag("bng2"), tag("beng")},
    {tag("gur3"), tag("gur2"), tag("guru")}, {tag("gjr3"), tag("gjr2"), tag("gujr")},
    {tag("ory3"), tag("ory2"), tag("orya")}, {tag("tml3"), tag("tml2"), tag("taml")},
    {tag("tel3"), tag("tel2"), tag("telu")}, {tag("knd3"), tag("knd2"), tag("knda")},
    {tag("mlm3"), tag("mlm2"), tag("mlym")}, {tag("sinh"), 0, 0},
};

unsigned bitStorage(unsigned v) {
  unsigned bits = 0;
  for (; v; v >>= 1) bits++;
  return bits;
}

// Binary search in a RecordList (count, then tag + offset records), as
// HarfBuzz's bfind; NOT_FOUND when absent.
unsigned findRecord(const Table& list, const uint32_t t) {
  int lo = 0;
  int hi = static_cast<int>(list.u16(0)) - 1;
  while (lo <= hi) {
    const int mid = static_cast<int>((static_cast<unsigned>(lo) + static_cast<unsigned>(hi)) / 2);
    const uint32_t k = list.u32(2 + 6 * mid);
    if (t < k) {
      hi = mid - 1;
    } else if (t > k) {
      lo = mid + 1;
    } else {
      return static_cast<unsigned>(mid);
    }
  }
  return NOT_FOUND;
}

// hb_ot_layout_table_select_script(): the requested tags, then DFLT, dflt,
// latn.
Table selectScript(const Table& layout, const uint32_t* tags, uint32_t* chosen) {
  const Table scripts = layout.offset16(4);
  unsigned index = NOT_FOUND;
  *chosen = 0;
  for (unsigned i = 0; i < 3 && index == NOT_FOUND; i++) {
    if (tags[i] && (index = findRecord(scripts, tags[i])) != NOT_FOUND) *chosen = tags[i];
  }
  for (const uint32_t fallback : {tag("DFLT"), tag("dflt"), tag("latn")}) {
    if (index == NOT_FOUND && (index = findRecord(scripts, fallback)) != NOT_FOUND) *chosen = fallback;
  }
  if (index == NOT_FOUND || index >= scripts.u16(0)) return Table();
  return scripts.offset16(2 + 6 * index + 4);
}

// hb_ot_layout_script_select_language(): the requested language systems,
// then dflt, then the script's default LangSys.
Table selectLangSys(const Table& script, const uint32_t* languageTags) {
  const Table records = script.at(2);  // langSysCount + records, a RecordList
  unsigned index = NOT_FOUND;
  for (unsigned i = 0; i < 3 && languageTags && languageTags[i] && index == NOT_FOUND; i++) {
    index = findRecord(records, languageTags[i]);
  }
  if (index == NOT_FOUND) index = findRecord(records, tag("dflt"));
  if (index == NOT_FOUND) return script.offset16(0);
  if (index >= script.u16(2)) return Table();
  return script.offset16(4 + 6 * index + 4);
}

uint32_t featureTag(const Table& layout, const unsigned featureIndex) {
  const Table features = layout.offset16(6);
  if (featureIndex >= features.u16(0)) return 0;
  return features.u32(2 + 6 * featureIndex);
}

// A FeatureVariations condition at the default instance (every normalized
// axis coordinate 0), as HarfBuzz evaluates it when no variations are set.
bool conditionHolds(const Table& condition, const unsigned depth = 0) {
  if (depth > 8) return false;
  switch (condition.u16(0)) {
    case 1:  // axis range
      return condition.s16(4) <= 0 && 0 <= condition.s16(6);
    case 2:  // value: the default plus a variation delta, which is zero here
      return condition.s16(2) > 0;
    case 3:    // and
    case 4: {  // or
      const bool isAnd = condition.u16(0) == 3;
      for (uint8_t i = 0; i < condition.u8(2); i++) {
        const bool holds = conditionHolds(condition.at(condition.u24(3 + 3 * i)), depth + 1);
        if (isAnd != holds) return holds;
      }
      return isAnd;
    }
    case 5:  // negate
      return !conditionHolds(condition.at(condition.u24(2)), depth + 1);
    default:
      return false;
  }
}

// The FeatureTableSubstitution of the first FeatureVariations record whose
// conditions hold at the default instance; empty when none does.
Table defaultFeatureSubstitution(const Table& layout) {
  if (layout.u16(2) < 1) return Table();
  const Table variations = layout.offset32(10);
  for (uint32_t i = 0; i < variations.u32(4); i++) {
    const uint32_t record = 8 + 8 * i;
    const Table set = variations.offset32(record);
    bool all = true;
    for (uint16_t c = 0; c < set.u16(0) && all; c++) all = conditionHolds(set.offset32(2 + 4 * c));
    if (all) return variations.offset32(record + 4);
  }
  return Table();
}

// The Feature table of `featureIndex`, as substituted at the default instance.
Table featureTable(const Table& layout, const Table& substitution, const unsigned featureIndex) {
  for (uint16_t i = 0; i < substitution.u16(4); i++) {
    if (substitution.u16(6 + 6 * i) == featureIndex) return substitution.offset32(6 + 6 * i + 2);
  }
  const Table features = layout.offset16(6);
  if (featureIndex >= features.u16(0)) return Table();
  return features.offset16(2 + 6 * featureIndex + 4);
}

}  // namespace

// hb_ot_map_builder_t's feature list: features in the order the shaper
// requests them, and the GSUB/GPOS stages (runs of lookups between pauses).
class FeatureBuilder {
 public:
  struct Info {
    uint32_t tag;
    unsigned seq;  // request order, for a stable sort
    unsigned maxValue;
    uint8_t flags;
    unsigned defaultValue;
    unsigned stage[2];
  };

  std::vector<Info> infos;
  std::vector<Pause> pauses[2];
  unsigned currentStage[2] = {0, 0};
  bool isSimple = true;  // no shaper features: HarfBuzz sorts only after merging

  void add(const uint32_t t, const uint8_t flags = 0, const unsigned value = 1) {
    infos.push_back(Info{t, static_cast<unsigned>(infos.size() + 1), value, flags,
                         (flags & ff::GLOBAL) ? value : 0, {currentStage[GSUB], currentStage[GPOS]}});
  }
  void enable(const uint32_t t, const uint8_t flags = 0, const unsigned value = 1) { add(t, flags | ff::GLOBAL, value); }
  void disable(const uint32_t t) { add(t, ff::GLOBAL, 0); }
  void pause(const int table, const Pause p) {
    pauses[table].push_back(p);
    currentStage[table]++;
  }

  // Merges repeated features (the earliest stage wins; a non-global request
  // makes the feature non-global).
  void sortAndMerge() {
    if (!isSimple) {
      std::stable_sort(infos.begin(), infos.end(), [](const Info& a, const Info& b) {
        return a.tag != b.tag ? a.tag < b.tag : a.seq < b.seq;
      });
    }
    if (infos.empty()) return;
    size_t j = 0;
    for (size_t i = 1; i < infos.size(); i++) {
      if (infos[i].tag != infos[j].tag) {
        infos[++j] = infos[i];
        continue;
      }
      if (infos[i].flags & ff::GLOBAL) {
        infos[j].flags |= ff::GLOBAL;
        infos[j].maxValue = infos[i].maxValue;
        infos[j].defaultValue = infos[i].defaultValue;
      } else {
        infos[j].flags &= ~ff::GLOBAL;
        infos[j].maxValue = std::max(infos[j].maxValue, infos[i].maxValue);
      }
      infos[j].flags |= infos[i].flags & ff::HAS_FALLBACK;
      infos[j].stage[GSUB] = std::min(infos[j].stage[GSUB], infos[i].stage[GSUB]);
      infos[j].stage[GPOS] = std::min(infos[j].stage[GPOS], infos[i].stage[GPOS]);
    }
    infos.resize(j + 1);
  }
};

namespace {

// hb_ot_shape_collect_features(), before the shaper's features. frac/numr/
// dnom and the private Harf/HARF/Buzz/BUZZ tags do nothing for Indic text;
// they are requested because HarfBuzz requests them, which fixes the stage
// and mask bit of every other feature.
void collectDefaultFeatures(FeatureBuilder& b) {
  b.enable(tag("rvrn"));
  b.pause(GSUB, Pause::None);
  b.enable(tag("ltra"));
  b.enable(tag("ltrm"));
  b.add(tag("frac"));
  b.add(tag("numr"));
  b.add(tag("dnom"));
  b.enable(tag("rand"), ff::RANDOM, MAX_FEATURE_VALUE);
  b.enable(tag("Harf"));
  b.enable(tag("HARF"));
}

// The common and horizontal features, after the shaper's.
void collectCommonFeatures(FeatureBuilder& b) {
  b.enable(tag("Buzz"));
  b.enable(tag("BUZZ"));
  b.add(tag("abvm"), ff::GLOBAL);
  b.add(tag("blwm"), ff::GLOBAL);
  b.add(tag("ccmp"), ff::GLOBAL);
  b.add(tag("locl"), ff::GLOBAL);
  b.add(tag("mark"), ff::GLOBAL_MANUAL_JOINERS);
  b.add(tag("mkmk"), ff::GLOBAL_MANUAL_JOINERS);
  b.add(tag("rlig"), ff::GLOBAL);
  b.add(tag("calt"), ff::GLOBAL);
  b.add(tag("clig"), ff::GLOBAL);
  b.add(tag("curs"), ff::GLOBAL);
  b.add(tag("dist"), ff::GLOBAL);
  b.add(tag("kern"), ff::GLOBAL_HAS_FALLBACK);
  b.add(tag("liga"), ff::GLOBAL);
  b.add(tag("rclt"), ff::GLOBAL);
}

// collect_features_indic(): each basic feature in its own stage, then the
// presentation features after final reordering.
void collectIndicFeatures(FeatureBuilder& b) {
  b.pause(GSUB, Pause::IndicSetupSyllables);
  b.enable(tag("locl"), ff::PER_SYLLABLE);
  b.enable(tag("ccmp"), ff::PER_SYLLABLE);
  b.pause(GSUB, Pause::IndicInitialReordering);
  static constexpr struct {
    uint32_t tag;
    uint8_t flags;
  } BASIC[] = {
      {tag("nukt"), ff::GLOBAL_MANUAL_JOINERS | ff::PER_SYLLABLE},
      {tag("akhn"), ff::GLOBAL_MANUAL_JOINERS | ff::PER_SYLLABLE},
      {tag("rphf"), ff::MANUAL_JOINERS | ff::PER_SYLLABLE},
      {tag("rkrf"), ff::GLOBAL_MANUAL_JOINERS | ff::PER_SYLLABLE},
      {tag("pref"), ff::MANUAL_JOINERS | ff::PER_SYLLABLE},
      {tag("blwf"), ff::MANUAL_JOINERS | ff::PER_SYLLABLE},
      {tag("abvf"), ff::MANUAL_JOINERS | ff::PER_SYLLABLE},
      {tag("half"), ff::MANUAL_JOINERS | ff::PER_SYLLABLE},
      {tag("pstf"), ff::MANUAL_JOINERS | ff::PER_SYLLABLE},
      {tag("vatu"), ff::GLOBAL_MANUAL_JOINERS | ff::PER_SYLLABLE},
      {tag("cjct"), ff::GLOBAL_MANUAL_JOINERS | ff::PER_SYLLABLE},
  };
  for (const auto& f : BASIC) {
    b.add(f.tag, f.flags);
    b.pause(GSUB, Pause::None);
  }
  b.pause(GSUB, Pause::IndicFinalReordering);
  static constexpr struct {
    uint32_t tag;
    uint8_t flags;
  } OTHER[] = {
      {tag("init"), ff::MANUAL_JOINERS | ff::PER_SYLLABLE},
      {tag("pres"), ff::GLOBAL_MANUAL_JOINERS | ff::PER_SYLLABLE},
      {tag("abvs"), ff::GLOBAL_MANUAL_JOINERS | ff::PER_SYLLABLE},
      {tag("blws"), ff::GLOBAL_MANUAL_JOINERS | ff::PER_SYLLABLE},
      {tag("psts"), ff::GLOBAL_MANUAL_JOINERS | ff::PER_SYLLABLE},
      {tag("haln"), ff::GLOBAL_MANUAL_JOINERS | ff::PER_SYLLABLE},
  };
  for (const auto& f : OTHER) b.add(f.tag, f.flags);
}

// override_features_indic(): no 'liga', and a final GSUB stage (HarfBuzz
// frees the syllable data there; no per-syllable lookup runs after it).
void overrideIndicFeatures(FeatureBuilder& b) {
  b.disable(tag("liga"));
  b.pause(GSUB, Pause::None);
}

// collect_features_use().
void collectUseFeatures(FeatureBuilder& b) {
  b.pause(GSUB, Pause::UseSetupSyllables);
  b.enable(tag("locl"), ff::PER_SYLLABLE);
  b.enable(tag("ccmp"), ff::PER_SYLLABLE);
  b.enable(tag("nukt"), ff::PER_SYLLABLE);
  b.enable(tag("akhn"), ff::MANUAL_ZWJ | ff::PER_SYLLABLE);
  b.pause(GSUB, Pause::UseClearSubstitutionFlags);
  b.add(tag("rphf"), ff::MANUAL_ZWJ | ff::PER_SYLLABLE);
  b.pause(GSUB, Pause::UseRecordRphf);
  b.pause(GSUB, Pause::UseClearSubstitutionFlags);
  b.enable(tag("pref"), ff::MANUAL_ZWJ | ff::PER_SYLLABLE);
  b.pause(GSUB, Pause::UseRecordPref);
  for (const uint32_t t : {tag("rkrf"), tag("abvf"), tag("blwf"), tag("half"), tag("pstf"), tag("vatu"), tag("cjct")}) {
    b.enable(t, ff::MANUAL_ZWJ | ff::PER_SYLLABLE);
  }
  b.pause(GSUB, Pause::UseReorder);
  b.pause(GSUB, Pause::None);  // HarfBuzz frees the syllable data here
  for (const uint32_t t : {tag("isol"), tag("init"), tag("medi"), tag("fina")}) b.add(t);
  b.pause(GSUB, Pause::None);
  for (const uint32_t t : {tag("abvs"), tag("blws"), tag("haln"), tag("pres"), tag("psts")}) {
    b.enable(t, ff::MANUAL_ZWJ);
  }
}

}  // namespace

bool Plan::build(const Face& face, const Script s, const uint32_t* languageTags) {
  script = s;
  LanguageSystem systems[2];
  selectLanguageSystems(face, languageTags, systems);
  chooseShaper(systems[GSUB].chosenScript);

  FeatureBuilder b;
  collectDefaultFeatures(b);
  if (shaper == ShaperKind::Indic) {
    b.isSimple = false;
    collectIndicFeatures(b);
  } else if (shaper == ShaperKind::Use) {
    b.isSimple = false;
    collectUseFeatures(b);
  }
  collectCommonFeatures(b);
  if (shaper == ShaperKind::Indic) overrideIndicFeatures(b);
  b.sortAndMerge();

  unsigned requiredStage[2] = {0, 0};
  mapFeatures(face, systems, b, requiredStage);
  b.pause(GSUB, Pause::None);
  b.pause(GPOS, Pause::None);
  if (!planLookups(face, systems, b, requiredStage)) return false;

  applyGpos = !face.layout(GPOS).empty();
  fallbackGlyphClasses = !face.hasGlyphClasses();
  zeroMarks = shaper != ShaperKind::Indic;
  adjustMarkPositioningWhenZeroing = !applyGpos;
  buildFilters(face);
  setupShaperData(face, systems[GSUB].chosenScript);
  return true;
}

void Plan::selectLanguageSystems(const Face& face, const uint32_t* languageTags, LanguageSystem* systems) const {
  for (int t = GSUB; t <= GPOS; t++) {
    LanguageSystem& ls = systems[t];
    const Table scriptT = selectScript(face.layout(t), SCRIPT_TAGS[static_cast<unsigned>(script)], &ls.chosenScript);
    ls.langSys = selectLangSys(scriptT, languageTags);
    ls.requiredIndex = ls.langSys.empty() ? NOT_FOUND : ls.langSys.u16(2);
    ls.requiredTag = ls.requiredIndex == NOT_FOUND ? 0 : featureTag(face.layout(t), ls.requiredIndex);
  }
}

// hb_ot_shaper_categorize().
void Plan::chooseShaper(const uint32_t chosenScript) {
  if (chosenScript == tag("DFLT") || chosenScript == tag("latn")) {
    shaper = ShaperKind::Default;
  } else if (script == Script::Sinhala || (chosenScript & 0xFF) == '3') {
    shaper = ShaperKind::Use;
  } else {
    shaper = ShaperKind::Indic;
  }
}

// hb_ot_map_builder_t::compile(), first half: which requested features the
// font has, and the mask bits each gets.
void Plan::mapFeatures(const Face& face, const LanguageSystem* systems, const FeatureBuilder& builder,
                       unsigned* requiredStage) {
  // The language system's features; earlier entries win.
  const auto findFeatureIndex = [&](const int t, const uint32_t featureT) -> unsigned {
    const Table& langSys = systems[t].langSys;
    for (uint16_t i = 0; i < langSys.u16(4); i++) {
      const uint16_t featureIndex = langSys.u16(6 + 2 * i);
      if (featureTag(face.layout(t), featureIndex) == featureT) return featureIndex;
    }
    return NOT_FOUND;
  };

  features_.clear();
  features_.reserve(builder.infos.size());
  globalMask = GLOBAL_BIT_MASK;
  unsigned nextBit = FIRST_FEATURE_BIT;
  for (const FeatureBuilder::Info& info : builder.infos) {
    const bool usesGlobalBit = (info.flags & ff::GLOBAL) && info.maxValue == 1;
    const unsigned bitsNeeded = usesGlobalBit ? 0 : std::min(MAX_BITS, bitStorage(info.maxValue));
    if (!info.maxValue || nextBit + bitsNeeded >= GLOBAL_BIT_SHIFT) continue;  // disabled, or out of bits
    FeatureMap m{};
    bool found = false;
    for (int t = GSUB; t <= GPOS; t++) {
      if (systems[t].requiredTag == info.tag) requiredStage[t] = info.stage[t];
      m.index[t] = findFeatureIndex(t, info.tag);
      found |= m.index[t] != NOT_FOUND;
    }
    if (!found && !(info.flags & ff::HAS_FALLBACK)) continue;
    m.tag = info.tag;
    m.stage[GSUB] = static_cast<uint8_t>(info.stage[GSUB]);
    m.stage[GPOS] = static_cast<uint8_t>(info.stage[GPOS]);
    m.lookupFlags = static_cast<uint8_t>((info.flags & ff::MANUAL_ZWNJ ? 0 : lookupbits::AUTO_ZWNJ) |
                                         (info.flags & ff::MANUAL_ZWJ ? 0 : lookupbits::AUTO_ZWJ) |
                                         (info.flags & ff::RANDOM ? lookupbits::RANDOM : 0) |
                                         (info.flags & ff::PER_SYLLABLE ? lookupbits::PER_SYLLABLE : 0));
    if (usesGlobalBit) {
      m.shift = GLOBAL_BIT_SHIFT;
      m.mask = GLOBAL_BIT_MASK;
    } else {
      m.shift = static_cast<uint8_t>(nextBit);
      m.mask = (1u << (nextBit + bitsNeeded)) - (1u << nextBit);
      nextBit += bitsNeeded;
      globalMask |= (info.defaultValue << m.shift) & m.mask;
    }
    features_.push_back(m);
  }
  if (builder.isSimple) {
    std::stable_sort(features_.begin(), features_.end(),
                     [](const FeatureMap& a, const FeatureMap& b) { return a.tag < b.tag; });
  }
}

// hb_ot_map_builder_t::compile(), second half: each stage's lookups, sorted
// by index with duplicates merged.
bool Plan::planLookups(const Face& face, const LanguageSystem* systems, const FeatureBuilder& builder,
                       const unsigned* requiredStage) {
  for (int t = GSUB; t <= GPOS; t++) {
    const Table& layout = face.layout(t);
    const Table substitution = defaultFeatureSubstitution(layout);
    const uint16_t lookupCount = face.lookupCount(t);
    const auto featureLookups = [&](const unsigned featureIndex) {
      return featureIndex == NOT_FOUND ? Table() : featureTable(layout, substitution, featureIndex);
    };

    // Reserve for every lookup the features list, so the list never grows.
    size_t total = featureLookups(systems[t].requiredIndex).u16(2);
    for (const FeatureMap& m : features_) total += featureLookups(m.index[t]).u16(2);
    if (total > MAX_PLANNED_LOOKUPS) return false;
    if (!heapAvailable(total * sizeof(PlannedLookup) + builder.currentStage[t] * sizeof(Stage))) return false;
    std::vector<PlannedLookup>& list = lookups[t];
    list.clear();
    list.reserve(total);
    stages[t].clear();
    stages[t].reserve(builder.currentStage[t]);

    const auto addLookups = [&](const unsigned featureIndex, const uint32_t mask, const uint8_t flags) {
      const Table feature = featureLookups(featureIndex);
      for (uint16_t i = 0; i < feature.u16(2); i++) {
        const uint16_t index = feature.u16(4 + 2 * i);
        if (index < lookupCount) list.push_back(PlannedLookup{index, flags, mask});
      }
    };

    size_t last = 0;
    for (unsigned stage = 0; stage < builder.currentStage[t]; stage++) {
      if (systems[t].requiredIndex != NOT_FOUND && requiredStage[t] == stage) {
        addLookups(systems[t].requiredIndex, GLOBAL_BIT_MASK, lookupbits::AUTO_ZWNJ | lookupbits::AUTO_ZWJ);
      }
      for (const FeatureMap& m : features_) {
        if (m.stage[t] == stage) addLookups(m.index[t], m.mask, m.lookupFlags);
      }
      if (last + 1 < list.size()) {
        std::stable_sort(list.begin() + static_cast<long>(last), list.end(),
                         [](const PlannedLookup& a, const PlannedLookup& b) { return a.index < b.index; });
        // A lookup in several features: masks unite, auto-joiner flags must
        // all agree, random and per-syllable keep the first.
        size_t j = last;
        for (size_t i = j + 1; i < list.size(); i++) {
          if (list[i].index != list[j].index) {
            list[++j] = list[i];
            continue;
          }
          list[j].mask |= list[i].mask;
          const uint8_t joiners = lookupbits::AUTO_ZWNJ | lookupbits::AUTO_ZWJ;
          list[j].flags = static_cast<uint8_t>((list[j].flags & ~joiners) | (list[j].flags & list[i].flags & joiners));
        }
        list.resize(j + 1);
      }
      last = list.size();
      stages[t].push_back(Stage{static_cast<uint16_t>(last), builder.pauses[t][stage]});
    }
  }
  return true;
}

// Lookup filters: from the font's CPac table when it has one, else computed
// on the heap when the heap check allows (else lookups run unfiltered).
void Plan::buildFilters(const Face& face) {
  for (int t = GSUB; t <= GPOS; t++) {
    lookupDigests_[t].clear();
    subtableStarts_[t].clear();
    subtableDigests_[t].clear();
    fontFilters_[t] = face.filters(t);
    if (fontFilters_[t]) continue;
    size_t subtables = 0;
    for (const PlannedLookup& lookup : lookups[t]) subtables += face.lookup(t, lookup.index).u16(4);
    const size_t bytes = (lookups[t].size() + subtables) * DIGEST_BYTES + lookups[t].size() * sizeof(uint32_t);
    if (!heapAvailable(bytes)) continue;
    lookupDigests_[t].resize(lookups[t].size() * DIGEST_BYTES);
    subtableStarts_[t].reserve(lookups[t].size());
    subtableDigests_[t].reserve(subtables * DIGEST_BYTES);
    for (size_t i = 0; i < lookups[t].size(); i++) {
      writeDigest(lookupDigest(face, t, lookups[t][i].index), lookupDigests_[t].data() + i * DIGEST_BYTES);
      subtableStarts_[t].push_back(static_cast<uint32_t>(subtableDigests_[t].size() / DIGEST_BYTES));
      appendSubtableDigests(face, t, lookups[t][i].index, subtableDigests_[t]);
    }
  }
}

void Plan::setupShaperData(const Face& face, const uint32_t chosenScript) {
  if (shaper == ShaperKind::Indic) {
    indicConfig = &INDIC_CONFIGS[static_cast<unsigned>(script)];
    isOldSpec = (chosenScript & 0xFF) != '2';
    // would_substitute() ignores context in new-spec fonts, except Malayalam
    // (as Uniscribe does, per HarfBuzz's data_create_indic()).
    zeroContext_ = !isOldSpec && script != Script::Malayalam;
    static constexpr uint32_t MASK_TAGS[INDIC_MASK_COUNT] = {tag("rphf"), tag("pref"), tag("blwf"), tag("abvf"),
                                                             tag("half"), tag("pstf"), tag("init")};
    for (int i = 0; i < INDIC_MASK_COUNT; i++) indicMasks[i] = oneMask(MASK_TAGS[i]);
    collectWould(face, WS_RPHF, tag("rphf"));
    collectWould(face, WS_PREF, tag("pref"));
    collectWould(face, WS_BLWF, tag("blwf"));
    collectWould(face, WS_PSTF, tag("pstf"));
    collectWould(face, WS_VATU, tag("vatu"));
  } else if (shaper == ShaperKind::Use) {
    useRphfMask = oneMask(tag("rphf"));
    const uint32_t topographical[4] = {tag("isol"), tag("init"), tag("medi"), tag("fina")};
    for (int i = 0; i < 4; i++) {
      useTopographicalMasks[i] = oneMask(topographical[i]);
      if (useTopographicalMasks[i] == globalMask) useTopographicalMasks[i] = 0;
    }
  }
}

LookupSettings Plan::settings(const int table, const size_t i) const {
  const PlannedLookup& lookup = lookups[table][i];
  LookupSettings out;
  out.mask = lookup.mask;
  out.autoZwnj = lookup.flags & lookupbits::AUTO_ZWNJ;
  out.autoZwj = lookup.flags & lookupbits::AUTO_ZWJ;
  out.random = lookup.flags & lookupbits::RANDOM;
  out.perSyllable = lookup.flags & lookupbits::PER_SYLLABLE;
  if (const FilterRecords* r = fontFilters_[table]) {
    out.hasDigest = true;
    out.digest = readDigest(r->lookupDigests + DIGEST_BYTES * lookup.index);
    out.subtableDigests = r->subtableDigests + DIGEST_BYTES * r->firstSubtable(lookup.index);
  } else if (!lookupDigests_[table].empty()) {
    out.hasDigest = true;
    out.digest = readDigest(lookupDigests_[table].data() + DIGEST_BYTES * i);
    out.subtableDigests = subtableDigests_[table].data() + DIGEST_BYTES * subtableStarts_[table][i];
  }
  return out;
}

uint32_t Plan::oneMask(const uint32_t t) const {
  for (const FeatureMap& f : features_) {
    if (f.tag == t) return (1u << f.shift) & f.mask;
  }
  return 0;
}

size_t Plan::memoryBytes() const {
  size_t bytes = sizeof(Plan) + features_.capacity() * sizeof(FeatureMap);
  for (int t = GSUB; t <= GPOS; t++) {
    bytes += lookups[t].capacity() * sizeof(PlannedLookup) + stages[t].capacity() * sizeof(Stage) +
             lookupDigests_[t].capacity() + subtableStarts_[t].capacity() * sizeof(uint32_t) +
             subtableDigests_[t].capacity();
  }
  for (const WouldSubstituteLookups& w : would_) {
    bytes += w.lookups.capacity() * sizeof(uint16_t) + w.digests.capacity() * sizeof(Digest);
  }
  return bytes;
}

// The lookups of the stage `tag` is applied in (get_stage_lookups()).
void Plan::collectWould(const Face& face, const WouldFeature feature, const uint32_t t) {
  WouldSubstituteLookups& w = would_[feature];
  w.lookups.clear();
  w.digests.clear();
  const FeatureMap* f = nullptr;
  for (const FeatureMap& candidate : features_) {
    if (candidate.tag == t) f = &candidate;
  }
  if (!f || f->stage[GSUB] > stages[GSUB].size()) return;
  const unsigned stage = f->stage[GSUB];
  const size_t start = stage ? stages[GSUB][stage - 1].lastLookup : 0;
  const size_t end = stage < stages[GSUB].size() ? stages[GSUB][stage].lastLookup : lookups[GSUB].size();
  w.lookups.reserve(end - start);
  w.digests.reserve(end - start);
  for (size_t i = start; i < end; i++) {
    w.lookups.push_back(lookups[GSUB][i].index);
    w.digests.push_back(lookupDigest(face, GSUB, lookups[GSUB][i].index));
  }
}

bool Plan::wouldSubstitute(const Face& face, const WouldFeature feature, const uint32_t* glyphs,
                           const unsigned count) const {
  const WouldSubstituteLookups& w = would_[feature];
  for (size_t i = 0; i < w.lookups.size(); i++) {
    if (ot::wouldSubstitute(face, w.lookups[i], w.digests[i], glyphs, count, zeroContext_)) return true;
  }
  return false;
}

}  // namespace ot
