#include "SdCardFontSystem.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <ttf/TtfFont.h>

#include <climits>
#include <cstdlib>
#include <cstring>

#include "CrossPointSettings.h"
#include "SdCardFontGlobals.h"
#include "activities/RenderLock.h"

static_assert(CrossPointSettings::FONT_SIZE_RUNGS[CrossPointSettings::FONT_SIZE_RUNG_COUNT - 1].points <=
                  TtfFont::kMaxPointSize,
              "TtfFont plans its scratch block for the largest reader size");

namespace {

// Between glyphs and install chunks: a hostile glyph can take most of a second, and an install
// several seconds, all under RenderLock. A tick's sleep every 200 ms lets the idle task run.
void ttfYield() {
  static uint32_t last = 0;
  const uint32_t now = millis();
  if (now - last < 200) return;
  last = now;
  vTaskDelay(1);
}

}  // namespace

// Free-function resolver used by CrossPointSettings::getReaderFontId().
// Resolved by the linker — no callback indirection stored in settings.
int resolveSdCardFontId(const char* familyName, uint8_t pointSize) {
  return sdFontSystem.resolveFontId(familyName, pointSize);
}

// --- Font-family dynamic SettingInfo trampolines ---
//
// The font-family SettingInfo lives in a namespace-static SettingsList that is
// initialized at global-static phase, well before sdFontSystem.begin() runs.
// We therefore cannot bake the SD family list into the SettingInfo at
// construction; instead the SettingInfo holds these stateless trampolines that
// consult sdFontSystem at every call. enumLabels is enriched lazily by the
// consumers (SettingsActivity, CrossPointWebServer) before each iteration.

uint8_t fontFamilyDynamicGetter(const void* /*ctx*/) {
  if (SETTINGS.sdFontFamilyName[0] != '\0') {
    const auto& families = sdFontSystem.registry().getFamilies();
    for (size_t i = 0; i < families.size(); i++) {
      if (families[i].name == SETTINGS.sdFontFamilyName) {
        return static_cast<uint8_t>(CrossPointSettings::BUILTIN_FONT_COUNT + i);
      }
    }
    // SD family no longer present (card removed?); fall through to built-in.
  }
  return SETTINGS.fontFamily < CrossPointSettings::BUILTIN_FONT_COUNT ? SETTINGS.fontFamily
                                                                      : CrossPointSettings::BOOKERLY;
}

void fontFamilyDynamicSetter(void* /*ctx*/, uint8_t value) {
  if (value < CrossPointSettings::BUILTIN_FONT_COUNT) {
    SETTINGS.fontFamily = value;
    SETTINGS.sdFontFamilyName[0] = '\0';
    return;
  }
  const auto& families = sdFontSystem.registry().getFamilies();
  uint8_t sdIdx = value - CrossPointSettings::BUILTIN_FONT_COUNT;
  if (sdIdx < families.size()) {
    strncpy(SETTINGS.sdFontFamilyName, families[sdIdx].name.c_str(), sizeof(SETTINGS.sdFontFamilyName) - 1);
    SETTINGS.sdFontFamilyName[sizeof(SETTINGS.sdFontFamilyName) - 1] = '\0';
  }
}

uint8_t txtFontFamilyDynamicGetter(const void* /*ctx*/) {
  if (SETTINGS.txtSdFontFamilyName[0] != '\0') {
    const auto& families = sdFontSystem.registry().getFamilies();
    for (size_t i = 0; i < families.size(); i++) {
      if (families[i].name == SETTINGS.txtSdFontFamilyName) {
        return static_cast<uint8_t>(CrossPointSettings::BUILTIN_FONT_COUNT + i);
      }
    }
  }
  return SETTINGS.txtFontFamily < CrossPointSettings::BUILTIN_FONT_COUNT ? SETTINGS.txtFontFamily
                                                                         : CrossPointSettings::NOTOSANS;
}

void txtFontFamilyDynamicSetter(void* /*ctx*/, uint8_t value) {
  if (value < CrossPointSettings::BUILTIN_FONT_COUNT) {
    SETTINGS.txtFontFamily = value;
    SETTINGS.txtSdFontFamilyName[0] = '\0';
    return;
  }
  const auto& families = sdFontSystem.registry().getFamilies();
  uint8_t sdIdx = value - CrossPointSettings::BUILTIN_FONT_COUNT;
  if (sdIdx < families.size()) {
    strncpy(SETTINGS.txtSdFontFamilyName, families[sdIdx].name.c_str(), sizeof(SETTINGS.txtSdFontFamilyName) - 1);
    SETTINGS.txtSdFontFamilyName[sizeof(SETTINGS.txtSdFontFamilyName) - 1] = '\0';
  }
}

uint8_t fontFamilyOptionCount() {
  return static_cast<uint8_t>(CrossPointSettings::BUILTIN_FONT_COUNT + sdFontSystem.registry().getFamilies().size());
}

std::string fontFamilyOptionLabel(uint8_t i) {
  if (i < CrossPointSettings::BUILTIN_FONT_COUNT) {
    static const StrId BUILTIN_LABELS[] = {StrId::STR_BOOKERLY, StrId::STR_NOTO_SANS};
    return I18N.get(BUILTIN_LABELS[i]);
  }
  const auto& families = sdFontSystem.registry().getFamilies();
  uint8_t sdIdx = i - CrossPointSettings::BUILTIN_FONT_COUNT;
  return sdIdx < families.size() ? families[sdIdx].name : std::string();
}

// --- Font-size dynamic SettingInfo trampolines ---
//
// The rows store a point size, but the settings UI deals in option indices, and the options are
// the sizes the selected family offers -- so both directions go through that family's list.

namespace {
uint8_t sizeIndex(const char* family, const uint8_t pointSize) {
  return sdFontSystem.sizeListFor(family).indexOf(pointSize);
}

void setSizeFromIndex(const char* family, uint8_t& pointSize, const uint8_t index) {
  const ReaderSizeList sizes = sdFontSystem.sizeListFor(family);
  if (index < sizes.count) pointSize = sizes.points[index];
}

uint8_t sizeOptionCount(const char* family) { return sdFontSystem.sizeListFor(family).count; }

std::string sizeOptionLabel(const char* family, const uint8_t index) {
  const ReaderSizeList sizes = sdFontSystem.sizeListFor(family);
  return index < sizes.count ? fontPointSizeLabel(sizes.points[index]) : std::string();
}
}  // namespace

uint8_t fontSizeDynamicGetter(const void* /*ctx*/) {
  return sizeIndex(SETTINGS.sdFontFamilyName, SETTINGS.fontPointSize);
}

void fontSizeDynamicSetter(void* /*ctx*/, const uint8_t index) {
  setSizeFromIndex(SETTINGS.sdFontFamilyName, SETTINGS.fontPointSize, index);
}

uint8_t txtFontSizeDynamicGetter(const void* /*ctx*/) {
  return sizeIndex(SETTINGS.txtSdFontFamilyName, SETTINGS.txtFontPointSize);
}

void txtFontSizeDynamicSetter(void* /*ctx*/, const uint8_t index) {
  setSizeFromIndex(SETTINGS.txtSdFontFamilyName, SETTINGS.txtFontPointSize, index);
}

uint8_t fontSizeOptionCount(const void* /*ctx*/) { return sizeOptionCount(SETTINGS.sdFontFamilyName); }

std::string fontSizeOptionLabel(const void* /*ctx*/, const uint8_t index) {
  return sizeOptionLabel(SETTINGS.sdFontFamilyName, index);
}

uint8_t txtFontSizeOptionCount(const void* /*ctx*/) { return sizeOptionCount(SETTINGS.txtSdFontFamilyName); }

std::string txtFontSizeOptionLabel(const void* /*ctx*/, const uint8_t index) {
  return sizeOptionLabel(SETTINGS.txtSdFontFamilyName, index);
}

const char* bookSdFontFamily(const int8_t builtinFamilyOverride, const std::string& sdFamilyOverride) {
  if (builtinFamilyOverride >= 0) return "";
  if (!sdFamilyOverride.empty()) return sdFamilyOverride.c_str();
  return SETTINGS.sdFontFamilyName;
}

ReaderSizeList SdCardFontSystem::sizeListFor(const char* familyName) const {
  if (!familyName || familyName[0] == '\0') return ReaderSizeList::builtin();
  // Names compared in place: findFamily() takes a std::string, and this runs while pages are laid
  // out, where building one for a long family name would allocate.
  for (const auto& family : registry_.getFamilies()) {
    if (family.name != familyName) continue;
    if (family.ttf) return ReaderSizeList::native();  // drawn natively at every size
    uint8_t sizes[ReaderSizeList::kCapacity];
    size_t n = 0;
    for (const auto& file : family.files) {
      if (n < ReaderSizeList::kCapacity) sizes[n++] = file.pointSize;
    }
    return ReaderSizeList::forFamily(sizes, n);
  }
  return ReaderSizeList::builtin();  // not on the card: the reader draws a built-in family
}

void SdCardFontSystem::begin(GfxRenderer& renderer) {
  (void)renderer;
  TtfFont::setHooks({&RenderLock::heldByCurrentTask, &ttfYield});
  registry_.discover();

  // Startup is discovery-only. SD font payloads are loaded lazily on reader
  // entry via ensureLoaded/ensureLoadedForPath and unloaded on reader exit.
  LOG_DBG("SDFS", "SD font system ready (discovery-only, %d families)", registry_.getFamilyCount());
}

void SdCardFontSystem::ensureLoaded(GfxRenderer& renderer) {
  const char* wantedFamily = SETTINGS.sdFontFamilyName;
  const std::string& currentFamily = manager_.currentFamilyName();
  const uint8_t targetPt = sizeListFor(wantedFamily).snap(SETTINGS.fontPointSize);

  if (wantedFamily[0] == '\0') {
    if (!currentFamily.empty()) {
      manager_.unloadAll(renderer);
    }
    return;
  }

  // Reload if family changed OR if the user-selected size changed and the
  // family has a closer file than what's currently loaded.
  bool familyMatches = (currentFamily == wantedFamily);
  if (familyMatches) {
    const auto* family = registry_.findFamily(wantedFamily);
    if (!family) {
      LOG_DBG("SDFS", "SD font family disappeared: %s (clearing)", wantedFamily);
      manager_.unloadAll(renderer);
      SETTINGS.sdFontFamilyName[0] = '\0';
      return;
    }
    if (family->ttf && manager_.ttfLoaded()) {
      manager_.setTtfBodySize(renderer, targetPt);  // drawn natively: a new size, never a reload
      return;
    }
    const auto* best = family->pickClosestSize(targetPt);
    const uint8_t bestPt = best ? best->pointSize : 0;
    if (bestPt == manager_.currentPointSize()) {
      // Right face already loaded; only the target size may have moved (a size change from
      // the reader menu lands here), and that is an alias, not a reload.
      manager_.ensureSizeAlias(renderer, targetPt);
      return;
    }
    LOG_DBG("SDFS", "Reloading %s: size %u -> %u (target %u)", wantedFamily, manager_.currentPointSize(), bestPt,
            targetPt);
  }

  if (!currentFamily.empty()) {
    manager_.unloadAll(renderer);
  }

  const auto* family = registry_.findFamily(wantedFamily);
  if (family) {
    if (manager_.loadFamily(*family, renderer, targetPt)) {
      LOG_DBG("SDFS", "Loaded SD font family: %s", wantedFamily);
    } else {
      LOG_ERR("SDFS", "Failed to load SD font family: %s (clearing)", wantedFamily);
      SETTINGS.sdFontFamilyName[0] = '\0';
    }
  } else {
    LOG_DBG("SDFS", "SD font family not found: %s (clearing)", wantedFamily);
    SETTINGS.sdFontFamilyName[0] = '\0';
  }
}

void SdCardFontSystem::ensureLoaded(GfxRenderer& renderer, const char* wantedFamily, const uint8_t pointSize,
                                    const std::function<void()>& onColdLoad, const FlashCachePolicy policy,
                                    BuildArena* const arena) {
  const std::string& currentFamily = manager_.currentFamilyName();
  const uint8_t targetPt = sizeListFor(wantedFamily).snap(pointSize);

  if (!wantedFamily || wantedFamily[0] == '\0') {
    if (!currentFamily.empty()) manager_.unloadAll(renderer);
    return;
  }

  bool familyMatches = (currentFamily == wantedFamily);
  if (familyMatches) {
    const auto* family = registry_.findFamily(wantedFamily);
    if (!family) {
      manager_.unloadAll(renderer);
      return;
    }
    if (family->ttf && manager_.ttfLoaded()) {
      // Book open: no glyph record of the family is in use, so a full store can be emptied here.
      manager_.wipeTtfIfPending(onColdLoad);
      manager_.setTtfBodySize(renderer, targetPt);
      return;
    }
    const auto* best = family->pickClosestSize(targetPt);
    if (best && best->pointSize == manager_.currentPointSize()) {
      manager_.ensureSizeAlias(renderer, targetPt);  // same face, possibly a new target size
      return;
    }
  }

  if (!currentFamily.empty()) manager_.unloadAll(renderer);

  const auto* family = registry_.findFamily(wantedFamily);
  if (!family) return;

  // The loader and the resolver used to disagree, and the loader lost. loadFamily() takes the
  // CLOSEST size the family ships, but resolveFontId() only handed the font out on an EXACT
  // point-size match -- so a family without the requested size was loaded into RAM and then never
  // used, and the reader rendered a built-in face instead. It became reachable when the ladder
  // grew past 18 pt, because no existing .cpfont ships anything larger.
  //
  // Both now agree: the closest face is loaded and the requested size is served from it, scaled,
  // under an alias ID (SdCardFontManager::ensureSizeAlias). The chosen typeface AND the chosen
  // size, which is what the setting promised.
  if (!manager_.loadFamily(*family, renderer, targetPt, onColdLoad, policy, arena)) {
    LOG_ERR("SDFS", "Failed to load SD font family: %s", wantedFamily);
  }
}

int SdCardFontSystem::resolveFontId(const char* familyName, const uint8_t pointSize) const {
  // The manager loads exactly one face for the active SD family and serves ONE target size from
  // it: the face's own, or a scaled alias made by ensureLoaded(). That target is the stored size
  // snapped to the family's list, exactly as ensureLoaded() snapped it. The settings row shows the
  // same snapped size, so a nearby size is never handed out without the reader seeing which.
  // Anything not prepared returns 0, and the caller falls back to a built-in family.
  if (!familyName || familyName[0] == '\0') return 0;
  return manager_.getFontId(familyName, sizeListFor(familyName).snap(pointSize));
}
