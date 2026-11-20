#include "OtPlan.h"

namespace ot {

namespace {

constexpr unsigned NOT_FOUND = 0xFFFF;

// OpenType script tags tried in order: Indic v3 (shaped by USE), v2, v1.
// Sinhala has one.
constexpr uint32_t SCRIPT_TAGS[][3] = {
    {tag("dev3"), tag("dev2"), tag("deva")}, {tag("bng3"), tag("bng2"), tag("beng")},
    {tag("gur3"), tag("gur2"), tag("guru")}, {tag("gjr3"), tag("gjr2"), tag("gujr")},
    {tag("ory3"), tag("ory2"), tag("orya")}, {tag("tml3"), tag("tml2"), tag("taml")},
    {tag("tel3"), tag("tel2"), tag("telu")}, {tag("knd3"), tag("knd2"), tag("knda")},
    {tag("mlm3"), tag("mlm2"), tag("mlym")}, {tag("sinh"), 0, 0},
};

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
Table selectLangSys(const Table& script, const uint32_t* languageTags, uint32_t* key) {
  const Table records = script.at(2);  // langSysCount + records, a RecordList
  unsigned index = NOT_FOUND;
  *key = 0;
  for (unsigned i = 0; i < 3 && languageTags && languageTags[i] && index == NOT_FOUND; i++) {
    if ((index = findRecord(records, languageTags[i])) != NOT_FOUND) *key = languageTags[i];
  }
  if (index == NOT_FOUND && (index = findRecord(records, tag("dflt"))) != NOT_FOUND) *key = tag("dflt");
  if (index == NOT_FOUND) return script.offset16(0);
  if (index >= script.u16(2)) return Table();
  return script.offset16(4 + 6 * index + 4);
}

// A compiled plan (docs/file-formats.md, CPpl).
namespace cppl {
constexpr uint16_t VERSION = 1;
constexpr uint32_t HEADER_BYTES = 4;
constexpr uint32_t RECORD_BYTES = 14;  // script u8, reserved u8, GSUB key u32, GPOS key u32, offset u32
constexpr uint32_t MASKS = 12;         // after chosenScript u32, shaper u8, reserved u8[3], globalMask u32
constexpr uint32_t WOULD_STAGES = MASKS + 4 * MF_COUNT;
constexpr uint32_t TABLES = WOULD_STAGES + 8;  // wouldStage u8[5], reserved u8[3]
constexpr uint32_t STAGE_BYTES = 4;            // lastLookup u16, pause u8, reserved u8
constexpr uint32_t LOOKUP_BYTES = 8;           // index u16, flags u8, reserved u8, mask u32
constexpr uint8_t MAX_PAUSE = static_cast<uint8_t>(Pause::UseReorder);

// Whether a stage may run `pause`: as the planner emits them, Indic pauses
// only in Indic plans and USE pauses only in USE plans, both after GSUB
// stages. Other pauses would run a shaper's code without its data.
bool pauseAllowed(const ShaperKind shaper, const int table, const uint8_t pause) {
  if (pause == static_cast<uint8_t>(Pause::None)) return true;
  if (table != GSUB || pause > MAX_PAUSE) return false;
  return pause <= static_cast<uint8_t>(Pause::IndicFinalReordering) ? shaper == ShaperKind::Indic
                                                                    : shaper == ShaperKind::Use;
}
}  // namespace cppl

}  // namespace

LanguageSystem selectLanguageSystem(const Table& layout, const Script script, const uint32_t* languageTags) {
  LanguageSystem ls;
  const Table scriptTable = selectScript(layout, SCRIPT_TAGS[static_cast<unsigned>(script)], &ls.chosenScript);
  ls.langSys = selectLangSys(scriptTable, languageTags, &ls.key);
  return ls;
}

bool Plan::load(const Face& face, const Script s, const uint32_t* languageTags) {
  if (read(face, s, languageTags)) return true;
  *this = Plan();  // safe to shape with: no lookups, no shaper
  return false;
}

bool Plan::read(const Face& face, const Script s, const uint32_t* languageTags) {
  const Table& compiled = face.compiledPlans();
  if (compiled.u16(0) != cppl::VERSION) return false;
  const uint32_t gsubKey = selectLanguageSystem(face.layout(GSUB), s, languageTags).key;
  const uint32_t gposKey = selectLanguageSystem(face.layout(GPOS), s, languageTags).key;
  Table body;
  for (uint16_t i = 0; i < compiled.u16(2) && body.empty(); i++) {
    const uint32_t record = cppl::HEADER_BYTES + cppl::RECORD_BYTES * i;
    if (compiled.u8(record) == static_cast<uint8_t>(s) && compiled.u32(record + 2) == gsubKey &&
        compiled.u32(record + 6) == gposKey) {
      body = compiled.at(compiled.u32(record + 10));
    }
  }
  if (body.empty() || body.u8(4) > static_cast<uint8_t>(ShaperKind::Use)) return false;
  // INDIC_CONFIGS has no entry for Sinhala, which shapes with USE.
  if (body.u8(4) == static_cast<uint8_t>(ShaperKind::Indic) &&
      static_cast<unsigned>(s) >= sizeof(INDIC_CONFIGS) / sizeof(INDIC_CONFIGS[0])) {
    return false;
  }

  script = s;
  chosenScript = body.u32(0);
  shaper = static_cast<ShaperKind>(body.u8(4));
  globalMask = body.u32(8);
  uint32_t masks[MF_COUNT];
  for (int i = 0; i < MF_COUNT; i++) masks[i] = body.u32(cppl::MASKS + 4 * i);
  setFeatureMasks(masks);
  for (int i = 0; i < WS_COUNT; i++) wouldStage[i] = body.u8(cppl::WOULD_STAGES + i);

  uint32_t at = cppl::TABLES;
  for (int t = GSUB; t <= GPOS; t++) {
    const uint16_t stageCount = body.u16(at);
    const uint16_t lookupCount = body.u16(at + 2);
    at += 4;
    const uint32_t bytes = stageCount * cppl::STAGE_BYTES + lookupCount * cppl::LOOKUP_BYTES;
    if (lookupCount > MAX_PLANNED_LOOKUPS || !body.has(at, bytes)) return false;
    if (!heapAvailable(lookupCount * sizeof(PlannedLookup) + stageCount * sizeof(Stage))) return false;
    stages[t].clear();
    stages[t].reserve(stageCount);
    for (uint16_t i = 0; i < stageCount; i++, at += cppl::STAGE_BYTES) {
      const Stage stage{body.u16(at), static_cast<Pause>(body.u8(at + 2))};
      const uint16_t previous = stages[t].empty() ? 0 : stages[t].back().lastLookup;
      if (stage.lastLookup < previous || stage.lastLookup > lookupCount ||
          !cppl::pauseAllowed(shaper, t, body.u8(at + 2))) {
        return false;
      }
      stages[t].push_back(stage);
    }
    lookups[t].clear();
    lookups[t].reserve(lookupCount);
    for (uint16_t i = 0; i < lookupCount; i++, at += cppl::LOOKUP_BYTES) {
      const uint16_t index = body.u16(at);
      if (index >= face.lookupCount(t)) return false;
      lookups[t].push_back(PlannedLookup{index, body.u8(at + 2), body.u32(at + 4)});
    }
  }
  finish(face);
  return true;
}

void Plan::setFeatureMasks(const uint32_t* masks) {
  for (int i = 0; i < INDIC_MASK_COUNT; i++) indicMasks[i] = masks[MF_RPHF + i];
  useRphfMask = masks[MF_RPHF];
  static constexpr MaskedFeature TOPOGRAPHICAL[4] = {MF_ISOL, MF_INIT, MF_MEDI, MF_FINA};
  for (int i = 0; i < 4; i++) {
    const uint32_t mask = masks[TOPOGRAPHICAL[i]];
    useTopographicalMasks[i] = mask == globalMask ? 0 : mask;
  }
}

void Plan::finish(const Face& face) {
  applyGpos = !face.layout(GPOS).empty();
  fallbackGlyphClasses = !face.hasGlyphClasses();
  zeroMarks = shaper != ShaperKind::Indic;
  adjustMarkPositioningWhenZeroing = !applyGpos;
  buildFilters(face);
  if (shaper == ShaperKind::Indic) {
    indicConfig = &INDIC_CONFIGS[static_cast<unsigned>(script)];
    isOldSpec = (chosenScript & 0xFF) != '2';
    // would_substitute() ignores context in new-spec fonts, except Malayalam
    // (as Uniscribe does, per HarfBuzz's data_create_indic()).
    zeroContext_ = !isOldSpec && script != Script::Malayalam;
    for (int feature = 0; feature < WS_COUNT; feature++) collectWould(static_cast<WouldFeature>(feature));
  }
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
    // 64-bit: planned lookups may share one lookup of 65535 subtables.
    uint64_t subtables = 0;
    for (const PlannedLookup& lookup : lookups[t]) subtables += Face::subtableCount(face.lookup(t, lookup.index));
    const uint64_t bytes = (lookups[t].size() + subtables) * DIGEST_BYTES + lookups[t].size() * sizeof(uint32_t);
    if (bytes > UINT32_MAX || !heapAvailable(static_cast<size_t>(bytes))) continue;
    lookupDigests_[t].resize(lookups[t].size() * DIGEST_BYTES);
    subtableStarts_[t].reserve(lookups[t].size());
    subtableDigests_[t].reserve(static_cast<size_t>(subtables) * DIGEST_BYTES);
    for (size_t i = 0; i < lookups[t].size(); i++) {
      writeDigest(lookupDigest(face, t, lookups[t][i].index), lookupDigests_[t].data() + i * DIGEST_BYTES);
      subtableStarts_[t].push_back(static_cast<uint32_t>(subtableDigests_[t].size() / DIGEST_BYTES));
      appendSubtableDigests(face, t, lookups[t][i].index, subtableDigests_[t]);
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

size_t Plan::memoryBytes() const {
  size_t bytes = sizeof(Plan);
  for (int t = GSUB; t <= GPOS; t++) {
    bytes += lookups[t].capacity() * sizeof(PlannedLookup) + stages[t].capacity() * sizeof(Stage) +
             lookupDigests_[t].capacity() + subtableStarts_[t].capacity() * sizeof(uint32_t) +
             subtableDigests_[t].capacity();
  }
  return bytes;
}

// The lookups of the stage the feature is applied in (get_stage_lookups()).
void Plan::collectWould(const WouldFeature feature) {
  WouldSubstituteLookups& w = would_[feature];
  w = WouldSubstituteLookups();
  const unsigned stage = wouldStage[feature];
  if (stage == NO_STAGE || stage > stages[GSUB].size()) return;
  // Stages end in order within lookups[GSUB] (read() and the builder check),
  // and a plan has at most MAX_PLANNED_LOOKUPS of them.
  w.start = stage ? stages[GSUB][stage - 1].lastLookup : 0;
  w.end = static_cast<uint16_t>(stage < stages[GSUB].size() ? stages[GSUB][stage].lastLookup : lookups[GSUB].size());
}

bool Plan::wouldSubstitute(const Face& face, const WouldFeature feature, const uint32_t* glyphs,
                           const unsigned count) const {
  // Without filters, every lookup may start at any glyph.
  static constexpr Digest ANY_GLYPH{{~0ull, ~0ull, ~0ull}};
  const WouldSubstituteLookups& w = would_[feature];
  for (size_t i = w.start; i < w.end; i++) {
    const LookupSettings s = settings(GSUB, i);
    if (ot::wouldSubstitute(face, lookups[GSUB][i].index, s.hasDigest ? s.digest : ANY_GLYPH, glyphs, count,
                            zeroContext_)) {
      return true;
    }
  }
  return false;
}

}  // namespace ot
