#include "EpubReaderMenuActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "BuildFeatures.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "SdCardFontGlobals.h"
#include "activities/settings/SettingActionDispatch.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace fui = freeink::ui;

// Keep in sync with the default in EpubReaderActivity.h — see the comment there for rationale.
#ifndef ENABLE_BENCHMARKS
#define ENABLE_BENCHMARKS 0
#endif

namespace {
// Three-state cycle helper for overrides represented as int8_t with -1 = default.
// Mirrors the helpers in QuickOverridesActivity.cpp; kept duplicated rather
// than extracted so each activity stays self-contained for 16 lines of code.
//   slot 0 -> -1 (default)
//   slot 1 -> 1  (on)
//   slot 2 -> 0  (off)
uint8_t threeStateSlotFromOverride(int8_t value) {
  if (value < 0) return 0;
  if (value > 0) return 1;
  return 2;
}

int8_t threeStateOverrideFromSlot(uint8_t slot) {
  if (slot == 0) return -1;
  if (slot == 1) return 1;
  return 0;
}

// Returns the localized name of the family currently used as the global default
// for the reader. When the user has selected an SD card font globally, the
// override menu's "Default" label should reflect that family by name even
// though the per-book override list itself is built-in only.
std::string defaultFontFamilyLabel(const SettingInfo& item) {
  if (SETTINGS.sdFontFamilyName[0] != '\0') {
    const auto& families = sdFontSystem.registry().getFamilies();
    const auto it = std::find_if(families.begin(), families.end(),
                                 [](const auto& family) { return family.name == SETTINGS.sdFontFamilyName; });
    if (it != families.end()) {
      return std::string(SETTINGS.sdFontFamilyName);
    }
  }
  // Built-in: index 0 is the "Default" entry, [1..] are built-in families
  // in CrossPointSettings::FONT_FAMILY order.
  return item.getEnumOptionLabel(static_cast<uint8_t>(SETTINGS.fontFamily + 1));
}
}  // namespace

ReaderSizeList EpubReaderMenuActivity::pendingSizeList() const {
  return sdFontSystem.sizeListFor(bookSdFontFamily(pendingFontFamilyOverride, pendingSdFontFamilyOverride));
}

EpubReaderMenuActivity::EpubReaderMenuActivity(
    GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title, const int currentPage,
    const int totalPages, const int bookProgressPercent, const uint8_t currentOrientation, const bool hasFootnotes,
    const int8_t initialEmbeddedStyleOverride, const int8_t initialImageRenderingOverride,
    const int8_t initialFontFamilyOverride, const std::string& initialSdFontFamilyOverride,
    const int8_t initialFontSizeOverride, const uint8_t initialTextDarkness, const bool initialBionicReadingOverride,
    const int8_t initialGuideDotsOverride, const int8_t initialParagraphAlignmentOverride,
    const int8_t initialTextAntiAliasingOverride, const int8_t initialHyphenationOverride,
    const int8_t initialFontSizeNormalizationOverride, const int8_t initialInlineFootnotePreviewsOverride,
    const bool hasStarredPages, const bool isCurrentPageStarred, const bool hasPrintedPages)
    : TabbedUiListActivity("EpubReaderMenu", renderer, mappedInput),
      currentPageStarred(isCurrentPageStarred),
      pendingOrientation(currentOrientation),
      pendingEmbeddedStyleOverride(initialEmbeddedStyleOverride),
      pendingImageRenderingOverride(initialImageRenderingOverride),
      pendingFontFamilyOverride(initialFontFamilyOverride),
      pendingSdFontFamilyOverride(initialSdFontFamilyOverride),
      pendingFontSizeOverride(initialFontSizeOverride),
      pendingTextDarkness(initialTextDarkness),
      pendingBionicReading(initialBionicReadingOverride),
      pendingGuideDotsOverride(initialGuideDotsOverride),
      pendingParagraphAlignmentOverride(initialParagraphAlignmentOverride),
      pendingTextAntiAliasingOverride(initialTextAntiAliasingOverride),
      pendingHyphenationOverride(initialHyphenationOverride),
      pendingFontSizeNormalizationOverride(initialFontSizeNormalizationOverride),
      pendingInlineFootnotePreviewsOverride(initialInlineFootnotePreviewsOverride),
      title(title),
      currentPage(currentPage),
      totalPages(totalPages),
      bookProgressPercent(bookProgressPercent) {
  buildMenuItems(hasFootnotes, hasStarredPages, hasPrintedPages);
}

void EpubReaderMenuActivity::buildMenuItems(bool hasFootnotes, bool hasStarredPages, bool hasPrintedPages) {
  auto& navigationItems = tabMenuItems[static_cast<size_t>(MenuTab::Navigation)];
  auto& settingsItems = tabMenuItems[static_cast<size_t>(MenuTab::Settings)];
  auto& syncItems = tabMenuItems[static_cast<size_t>(MenuTab::Sync)];
  auto& toolsItems = tabMenuItems[static_cast<size_t>(MenuTab::Tools)];
  navigationItems.reserve(9);
  settingsItems.reserve(13);
  syncItems.reserve(3);
  toolsItems.reserve(8);

  visibleTabs[visibleTabCount++] = MenuTab::Navigation;
  visibleTabs[visibleTabCount++] = MenuTab::Settings;
  if (KOREADER_STORE.hasCredentials()) visibleTabs[visibleTabCount++] = MenuTab::Sync;
  visibleTabs[visibleTabCount++] = MenuTab::Tools;

  navigationItems.push_back(SettingInfo::Action(StrId::STR_SELECT_CHAPTER, SettingAction::None));
  navigationItems.push_back(SettingInfo::Action(StrId::STR_SEARCH_BOOK, SettingAction::None));
  navigationItems.push_back(SettingInfo::Action(StrId::STR_GO_TO_PERCENT, SettingAction::None));
  if (hasPrintedPages) {
    navigationItems.push_back(SettingInfo::Action(StrId::STR_GO_TO_PRINTED_PAGE, SettingAction::None));
  }
  // Auto page turn: ACTION type with custom cycling in onActionSelected
  navigationItems.push_back(SettingInfo::Action(StrId::STR_AUTO_TURN_PAGES_PER_MIN, SettingAction::None));
  navigationItems.push_back(SettingInfo::Action(StrId::STR_STAR_PAGE, SettingAction::None));
  if (hasStarredPages) {
    navigationItems.push_back(SettingInfo::Action(StrId::STR_STARRED_PAGES, SettingAction::None));
  }
  if (hasFootnotes) {
    navigationItems.push_back(SettingInfo::Action(StrId::STR_FOOTNOTES, SettingAction::None));
  }
  // Always offered, even with no dictionary configured: choosing it then opens
  // the picker, which is more useful than hiding the feature from the person
  // who has not found the setting yet.
#if CP_FEATURE_DICTIONARY
  navigationItems.push_back(SettingInfo::Action(StrId::STR_DICTIONARY, SettingAction::None));
#endif

  auto* self = this;
  // Orientation: straightforward 0-3 cycle
  settingsItems.push_back(SettingInfo::DynamicEnumCtx(
      StrId::STR_ORIENTATION,
      {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_INVERTED, StrId::STR_LANDSCAPE_CCW}, self,
      [](const void* ctx) -> uint8_t { return static_cast<const EpubReaderMenuActivity*>(ctx)->pendingOrientation; },
      [](void* ctx, uint8_t v) { static_cast<EpubReaderMenuActivity*>(ctx)->pendingOrientation = v; }));

  // Embedded style: cycles default(-1) -> ON(1) -> OFF(0) via DynamicEnum indices 0/1/2
  settingsItems.push_back(SettingInfo::DynamicEnumCtx(
      StrId::STR_EMBEDDED_STYLE, {StrId::STR_DEFAULT_VALUE, StrId::STR_STATE_ON, StrId::STR_STATE_OFF}, self,
      [](const void* ctx) -> uint8_t {
        const auto* s = static_cast<const EpubReaderMenuActivity*>(ctx);
        if (s->pendingEmbeddedStyleOverride < 0) return 0;
        if (s->pendingEmbeddedStyleOverride > 0) return 1;
        return 2;
      },
      [](void* ctx, uint8_t v) {
        auto* s = static_cast<EpubReaderMenuActivity*>(ctx);
        if (v == 0)
          s->pendingEmbeddedStyleOverride = -1;
        else if (v == 1)
          s->pendingEmbeddedStyleOverride = 1;
        else
          s->pendingEmbeddedStyleOverride = 0;
      }));

  // Image rendering: cycles default(-1) -> display(0) -> placeholder(1) -> suppress(2)
  settingsItems.push_back(SettingInfo::DynamicEnumCtx(
      StrId::STR_IMAGES,
      {StrId::STR_DEFAULT_VALUE, StrId::STR_IMAGES_DISPLAY, StrId::STR_IMAGES_PLACEHOLDER, StrId::STR_IMAGES_SUPPRESS},
      self,
      [](const void* ctx) -> uint8_t {
        const auto* s = static_cast<const EpubReaderMenuActivity*>(ctx);
        return (s->pendingImageRenderingOverride < 0) ? 0 : (s->pendingImageRenderingOverride + 1);
      },
      [](void* ctx, uint8_t v) {
        auto* s = static_cast<EpubReaderMenuActivity*>(ctx);
        s->pendingImageRenderingOverride = (v == 0) ? -1 : static_cast<int8_t>(v - 1);
      }));

  // Reader font family: default + built-ins + discovered SD families.
  {
    std::vector<StrId> values = {StrId::STR_DEFAULT_VALUE, StrId::STR_BOOKERLY, StrId::STR_NOTO_SANS};
    const auto& families = sdFontSystem.registry().getFamilies();
    values.insert(values.end(), families.size(), StrId::STR_NONE_OPT);

    auto familySetting = SettingInfo::DynamicEnumCtx(
                             StrId::STR_FONT_FAMILY, values, self,
                             [](const void* ctx) -> uint8_t {
                               const auto* s = static_cast<const EpubReaderMenuActivity*>(ctx);
                               if (s->pendingFontFamilyOverride >= 0) {
                                 return static_cast<uint8_t>(s->pendingFontFamilyOverride + 1);
                               }
                               if (!s->pendingSdFontFamilyOverride.empty()) {
                                 const auto& fs = sdFontSystem.registry().getFamilies();
                                 for (size_t i = 0; i < fs.size(); i++) {
                                   if (fs[i].name == s->pendingSdFontFamilyOverride) {
                                     return static_cast<uint8_t>(CrossPointSettings::BUILTIN_FONT_COUNT + 1 + i);
                                   }
                                 }
                               }
                               return 0;
                             },
                             [](void* ctx, uint8_t v) {
                               auto* s = static_cast<EpubReaderMenuActivity*>(ctx);
                               if (v == 0) {
                                 s->pendingFontFamilyOverride = -1;
                                 s->pendingSdFontFamilyOverride.clear();
                                 return;
                               }

                               const uint8_t builtinCount = CrossPointSettings::BUILTIN_FONT_COUNT;
                               if (v <= builtinCount) {
                                 s->pendingFontFamilyOverride = static_cast<int8_t>(v - 1);
                                 s->pendingSdFontFamilyOverride.clear();
                                 return;
                               }

                               const size_t sdIdx = static_cast<size_t>(v - (builtinCount + 1));
                               const auto& fs = sdFontSystem.registry().getFamilies();
                               s->pendingFontFamilyOverride = -1;
                               if (sdIdx < fs.size()) {
                                 s->pendingSdFontFamilyOverride = fs[sdIdx].name;
                               } else {
                                 s->pendingSdFontFamilyOverride.clear();
                               }
                             })
                             .withSelectorActivity();

    familySetting.enumLabels = {tr(STR_DEFAULT_VALUE), tr(STR_BOOKERLY), tr(STR_NOTO_SANS)};
    for (const auto& fam : families) {
      familySetting.enumLabels.push_back(fam.name);
    }
    settingsItems.push_back(std::move(familySetting));
  }

  // Reader font size: Default, then the sizes the pending family offers. The override is stored as
  // a point size; option i + 1 is entry i of pendingSizeList().
  auto fontSizeSetting =
      SettingInfo::DynamicEnumCtx(
          StrId::STR_FONT_SIZE, {}, self,
          [](const void* ctx) -> uint8_t {
            const auto* s = static_cast<const EpubReaderMenuActivity*>(ctx);
            if (s->pendingFontSizeOverride < 0) return 0;
            return static_cast<uint8_t>(1 +
                                        s->pendingSizeList().indexOf(static_cast<uint8_t>(s->pendingFontSizeOverride)));
          },
          [](void* ctx, uint8_t v) {
            auto* s = static_cast<EpubReaderMenuActivity*>(ctx);
            if (v == 0) {
              s->pendingFontSizeOverride = -1;
              return;
            }
            const ReaderSizeList sizes = s->pendingSizeList();
            if (v - 1 < sizes.count) s->pendingFontSizeOverride = static_cast<int8_t>(sizes.points[v - 1]);
          })
          .withDynamicOptions(
              [](const void* ctx) -> uint8_t {
                return static_cast<uint8_t>(1 +
                                            static_cast<const EpubReaderMenuActivity*>(ctx)->pendingSizeList().count);
              },
              [](const void* ctx, const uint8_t index) -> std::string {
                if (index == 0) return tr(STR_DEFAULT_VALUE);
                const ReaderSizeList sizes = static_cast<const EpubReaderMenuActivity*>(ctx)->pendingSizeList();
                return index - 1 < sizes.count ? fontPointSizeLabel(sizes.points[index - 1]) : std::string();
              })
          .withSelectorActivity();
  settingsItems.push_back(std::move(fontSizeSetting));

  // Text darkness. The list is positional -- index IS the stored value -- and
  // Lighter sits last despite being the lightest, because the value is
  // persisted and inserting it at 0 would redefine every saved choice. Same
  // order as the Settings list, deliberately: two different orderings for one
  // setting would be worse than one odd one.
  settingsItems.push_back(
      SettingInfo::DynamicEnumCtx(
          StrId::STR_TEXT_DARKNESS,
          {StrId::STR_NORMAL, StrId::STR_DARK, StrId::STR_EXTRA_DARK, StrId::STR_MAX_DARK, StrId::STR_LIGHTER}, self,
          [](const void* ctx) -> uint8_t {
            return static_cast<const EpubReaderMenuActivity*>(ctx)->pendingTextDarkness;
          },
          [](void* ctx, uint8_t v) { static_cast<EpubReaderMenuActivity*>(ctx)->pendingTextDarkness = v; })
          .withSelectorActivity());

  settingsItems.push_back(SettingInfo::DynamicEnumCtx(
      StrId::STR_BIONIC_READING, {StrId::STR_STATE_OFF, StrId::STR_STATE_ON}, self,
      [](const void* ctx) -> uint8_t {
        return static_cast<const EpubReaderMenuActivity*>(ctx)->pendingBionicReading ? 1 : 0;
      },
      [](void* ctx, uint8_t v) { static_cast<EpubReaderMenuActivity*>(ctx)->pendingBionicReading = (v != 0); }));

  // Guide dots: default / on / off (mirrors QuickOverrides)
  settingsItems.push_back(SettingInfo::DynamicEnumCtx(
      StrId::STR_GUIDE_DOTS, {StrId::STR_DEFAULT_VALUE, StrId::STR_STATE_ON, StrId::STR_STATE_OFF}, self,
      [](const void* ctx) -> uint8_t {
        return threeStateSlotFromOverride(static_cast<const EpubReaderMenuActivity*>(ctx)->pendingGuideDotsOverride);
      },
      [](void* ctx, uint8_t v) {
        static_cast<EpubReaderMenuActivity*>(ctx)->pendingGuideDotsOverride = threeStateOverrideFromSlot(v);
      }));

  // Paragraph alignment: default(-1) + the 5 global options
  settingsItems.push_back(SettingInfo::DynamicEnumCtx(
                              StrId::STR_PARA_ALIGNMENT,
                              {StrId::STR_DEFAULT_VALUE, StrId::STR_JUSTIFY, StrId::STR_ALIGN_LEFT, StrId::STR_CENTER,
                               StrId::STR_ALIGN_RIGHT, StrId::STR_BOOK_S_STYLE},
                              self,
                              [](const void* ctx) -> uint8_t {
                                const auto* s = static_cast<const EpubReaderMenuActivity*>(ctx);
                                return (s->pendingParagraphAlignmentOverride < 0)
                                           ? 0
                                           : static_cast<uint8_t>(s->pendingParagraphAlignmentOverride + 1);
                              },
                              [](void* ctx, uint8_t v) {
                                auto* s = static_cast<EpubReaderMenuActivity*>(ctx);
                                s->pendingParagraphAlignmentOverride = (v == 0) ? -1 : static_cast<int8_t>(v - 1);
                              })
                              .withSelectorActivity());

  // Text anti-aliasing: default / on / off (mirrors QuickOverrides)
  settingsItems.push_back(SettingInfo::DynamicEnumCtx(
      StrId::STR_TEXT_AA, {StrId::STR_DEFAULT_VALUE, StrId::STR_STATE_ON, StrId::STR_STATE_OFF}, self,
      [](const void* ctx) -> uint8_t {
        return threeStateSlotFromOverride(
            static_cast<const EpubReaderMenuActivity*>(ctx)->pendingTextAntiAliasingOverride);
      },
      [](void* ctx, uint8_t v) {
        static_cast<EpubReaderMenuActivity*>(ctx)->pendingTextAntiAliasingOverride = threeStateOverrideFromSlot(v);
      }));

  // Hyphenation: default / on / off (mirrors QuickOverrides)
  settingsItems.push_back(SettingInfo::DynamicEnumCtx(
      StrId::STR_HYPHENATION, {StrId::STR_DEFAULT_VALUE, StrId::STR_STATE_ON, StrId::STR_STATE_OFF}, self,
      [](const void* ctx) -> uint8_t {
        return threeStateSlotFromOverride(static_cast<const EpubReaderMenuActivity*>(ctx)->pendingHyphenationOverride);
      },
      [](void* ctx, uint8_t v) {
        static_cast<EpubReaderMenuActivity*>(ctx)->pendingHyphenationOverride = threeStateOverrideFromSlot(v);
      }));

  // Font size normalization: default / on / off (mirrors QuickOverrides)
  settingsItems.push_back(SettingInfo::DynamicEnumCtx(
      StrId::STR_FONT_SIZE_NORMALIZATION, {StrId::STR_DEFAULT_VALUE, StrId::STR_STATE_ON, StrId::STR_STATE_OFF}, self,
      [](const void* ctx) -> uint8_t {
        return threeStateSlotFromOverride(
            static_cast<const EpubReaderMenuActivity*>(ctx)->pendingFontSizeNormalizationOverride);
      },
      [](void* ctx, uint8_t v) {
        static_cast<EpubReaderMenuActivity*>(ctx)->pendingFontSizeNormalizationOverride = threeStateOverrideFromSlot(v);
      }));

  settingsItems.push_back(SettingInfo::DynamicEnumCtx(
      StrId::STR_INLINE_FOOTNOTE_PREVIEWS, {StrId::STR_DEFAULT_VALUE, StrId::STR_STATE_ON, StrId::STR_STATE_OFF}, self,
      [](const void* ctx) -> uint8_t {
        return threeStateSlotFromOverride(
            static_cast<const EpubReaderMenuActivity*>(ctx)->pendingInlineFootnotePreviewsOverride);
      },
      [](void* ctx, uint8_t v) {
        static_cast<EpubReaderMenuActivity*>(ctx)->pendingInlineFootnotePreviewsOverride =
            threeStateOverrideFromSlot(v);
      }));

  if (KOREADER_STORE.hasCredentials()) {
    syncItems.push_back(SettingInfo::Action(StrId::STR_PULL_PROGRESS_FROM_OTHER_DEVICES, SettingAction::None));
    syncItems.push_back(SettingInfo::Action(StrId::STR_PUSH_PROGRESS_FROM_THIS_DEVICE, SettingAction::None));
    // The same comparison a long Confirm press in the reader starts: show which side is further and
    // let the user choose, instead of forcing one direction.
    syncItems.push_back(SettingInfo::Action(StrId::STR_COMPARE_PROGRESS_WITH_OTHER_DEVICES, SettingAction::None));
  }

  toolsItems.push_back(SettingInfo::Action(StrId::STR_BOOK_INFO, SettingAction::None));
  toolsItems.push_back(SettingInfo::Action(StrId::STR_READING_STATS_FOR_THIS_BOOK, SettingAction::None));
  toolsItems.push_back(SettingInfo::Action(StrId::STR_MARK_AS_READ, SettingAction::None));
  toolsItems.push_back(SettingInfo::Action(StrId::STR_DELETE_CACHE, SettingAction::None));
#if CP_FEATURE_SCREENSHOT
  toolsItems.push_back(SettingInfo::Action(StrId::STR_SCREENSHOT_BUTTON, SettingAction::None));
#endif
#if CP_FEATURE_QR_PAGE
  toolsItems.push_back(SettingInfo::Action(StrId::STR_DISPLAY_QR, SettingAction::None));
#endif
  toolsItems.push_back(SettingInfo::Action(StrId::STR_GO_HOME_BUTTON, SettingAction::None));
#if ENABLE_BENCHMARKS
  toolsItems.push_back(SettingInfo::Action(StrId::STR_RENDER_BENCHMARK, SettingAction::None));
#endif  // ENABLE_BENCHMARKS
}

EpubReaderMenuActivity::MenuAction EpubReaderMenuActivity::actionForNameId(StrId nameId) {
  switch (nameId) {
    case StrId::STR_SELECT_CHAPTER:
      return MenuAction::SELECT_CHAPTER;
    case StrId::STR_SEARCH_BOOK:
      return MenuAction::SEARCH;
    case StrId::STR_GO_TO_PERCENT:
      return MenuAction::GO_TO_PERCENT;
    case StrId::STR_GO_TO_PRINTED_PAGE:
      return MenuAction::GO_TO_PRINTED_PAGE;
    case StrId::STR_STARRED_PAGES:
      return MenuAction::STARRED_PAGES;
    case StrId::STR_STAR_PAGE:
      return MenuAction::STAR_PAGE;
    case StrId::STR_FOOTNOTES:
      return MenuAction::FOOTNOTES;
    case StrId::STR_DICTIONARY:
      return MenuAction::DICTIONARY;
    case StrId::STR_AUTO_TURN_PAGES_PER_MIN:
      return MenuAction::AUTO_PAGE_TURN;
    case StrId::STR_EMBEDDED_STYLE:
      return MenuAction::EMBEDDED_STYLE;
    case StrId::STR_IMAGES:
      return MenuAction::IMAGE_RENDERING;
    case StrId::STR_TEXT_DARKNESS:
      return MenuAction::TEXT_DARKNESS;
    case StrId::STR_ORIENTATION:
      return MenuAction::ROTATE_SCREEN;
    case StrId::STR_PULL_PROGRESS_FROM_OTHER_DEVICES:
      return MenuAction::PULL_REMOTE;
    case StrId::STR_PUSH_PROGRESS_FROM_THIS_DEVICE:
      return MenuAction::PUSH_LOCAL;
    case StrId::STR_COMPARE_PROGRESS_WITH_OTHER_DEVICES:
      return MenuAction::COMPARE_REMOTE;
    case StrId::STR_SCREENSHOT_BUTTON:
      return MenuAction::SCREENSHOT;
    case StrId::STR_DISPLAY_QR:
      return MenuAction::DISPLAY_QR;
    case StrId::STR_MARK_AS_READ:
      return MenuAction::MARK_AS_READ;
    case StrId::STR_DELETE_CACHE:
      return MenuAction::DELETE_CACHE;
    case StrId::STR_RENDER_BENCHMARK:
      return MenuAction::RENDER_BENCHMARK;
    case StrId::STR_GO_HOME_BUTTON:
      return MenuAction::GO_HOME;
    case StrId::STR_READING_STATS_FOR_THIS_BOOK:
      return MenuAction::READING_STATS_FOR_BOOK;
    case StrId::STR_BOOK_INFO:
      return MenuAction::BOOK_INFO;
    default:
      return MenuAction::NONE;
  }
}

void EpubReaderMenuActivity::finishWithAction(MenuAction action) {
  MenuResult payload{static_cast<int>(action),
                     -1,
                     pendingOrientation,
                     selectedPageTurnOption,
                     pendingEmbeddedStyleOverride,
                     pendingImageRenderingOverride,
                     pendingFontFamilyOverride,
                     pendingSdFontFamilyOverride,
                     pendingFontSizeOverride,
                     pendingTextDarkness,
                     static_cast<uint8_t>(pendingBionicReading),
                     pendingParagraphAlignmentOverride,
                     pendingTextAntiAliasingOverride,
                     pendingHyphenationOverride};
  // Appended after the file-browser fields, so set by name rather than position.
  payload.fontSizeNormalizationOverride = pendingFontSizeNormalizationOverride;
  payload.guideDotsOverride = pendingGuideDotsOverride;
  payload.inlineFootnotePreviewsOverride = pendingInlineFootnotePreviewsOverride;
  setResult(std::move(payload));
  finish();
}

void EpubReaderMenuActivity::onActionSelected(int index) {
  const auto& item = activeMenuItems()[index];

  // Auto page turn cycles locally (not a DynamicEnum because labels are raw strings)
  if (item.nameId == StrId::STR_AUTO_TURN_PAGES_PER_MIN) {
    selectedPageTurnOption = (selectedPageTurnOption + 1) % 5;
    requestUpdate();
    return;
  }

  // All other ACTION items finish with a result
  finishWithAction(actionForNameId(item.nameId));
}

void EpubReaderMenuActivity::onSettingToggled(int /*index*/) {
  // DynamicEnum items update pending state via their setters — no persistence needed.
}

void EpubReaderMenuActivity::onBackPressed() {
  ActivityResult result;
  result.isCancelled = true;
  MenuResult payload{-1,
                     -1,
                     pendingOrientation,
                     selectedPageTurnOption,
                     pendingEmbeddedStyleOverride,
                     pendingImageRenderingOverride,
                     pendingFontFamilyOverride,
                     pendingSdFontFamilyOverride,
                     pendingFontSizeOverride,
                     pendingTextDarkness,
                     static_cast<uint8_t>(pendingBionicReading),
                     pendingParagraphAlignmentOverride,
                     pendingTextAntiAliasingOverride,
                     pendingHyphenationOverride};
  // Appended after the file-browser fields, so set by name rather than position.
  payload.fontSizeNormalizationOverride = pendingFontSizeNormalizationOverride;
  payload.guideDotsOverride = pendingGuideDotsOverride;
  payload.inlineFootnotePreviewsOverride = pendingInlineFootnotePreviewsOverride;
  result.data = std::move(payload);
  setResult(std::move(result));
  finish();
}

std::string EpubReaderMenuActivity::getItemValueString(int index) const {
  const auto& item = activeMenuItems()[index];

  // Auto page turn: custom labels
  if (item.nameId == StrId::STR_AUTO_TURN_PAGES_PER_MIN) {
    if (selectedPageTurnOption == 0) return std::string(tr(STR_STATE_OFF));
    return std::string(pageTurnLabels[selectedPageTurnOption]);
  }

  // Star page: reflect current page's star state
  if (item.nameId == StrId::STR_STAR_PAGE) {
    return currentPageStarred ? std::string(tr(STR_STATE_ON)) : std::string(tr(STR_STATE_OFF));
  }

  if (item.type == SettingType::ACTION) {
    return {};
  }

  if (item.type == SettingType::ENUM) {
    if (item.nameId == StrId::STR_EMBEDDED_STYLE && pendingEmbeddedStyleOverride < 0) {
      const auto defaultEffective = (SETTINGS.embeddedStyle != 0) ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
      return std::string(tr(STR_DEFAULT_VALUE)) + " (" + defaultEffective + ")";
    }
    if (item.nameId == StrId::STR_IMAGES && pendingImageRenderingOverride < 0) {
      const auto label = item.getEnumOptionLabel(static_cast<uint8_t>(SETTINGS.imageRendering + 1));
      if (!label.empty()) return std::string(tr(STR_DEFAULT_VALUE)) + " (" + label + ")";
    }
    if (item.nameId == StrId::STR_FONT_FAMILY && pendingFontFamilyOverride < 0 && pendingSdFontFamilyOverride.empty()) {
      const auto label = defaultFontFamilyLabel(item);
      if (!label.empty()) {
        return std::string(tr(STR_DEFAULT_VALUE)) + " (" + label + ")";
      }
    }
    if (item.nameId == StrId::STR_FONT_SIZE && pendingFontSizeOverride < 0) {
      const auto label =
          item.getEnumOptionLabel(static_cast<uint8_t>(1 + pendingSizeList().indexOf(SETTINGS.fontPointSize)));
      if (!label.empty()) return std::string(tr(STR_DEFAULT_VALUE)) + " (" + label + ")";
    }
    if (item.nameId == StrId::STR_PARA_ALIGNMENT && pendingParagraphAlignmentOverride < 0) {
      const auto label = item.getEnumOptionLabel(static_cast<uint8_t>(SETTINGS.paragraphAlignment + 1));
      if (!label.empty()) return std::string(tr(STR_DEFAULT_VALUE)) + " (" + label + ")";
    }
    if (item.nameId == StrId::STR_TEXT_AA && pendingTextAntiAliasingOverride < 0) {
      const auto defaultEffective = (SETTINGS.textAntiAliasing != 0) ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
      return std::string(tr(STR_DEFAULT_VALUE)) + " (" + defaultEffective + ")";
    }
    if (item.nameId == StrId::STR_HYPHENATION && pendingHyphenationOverride < 0) {
      const auto defaultEffective = (SETTINGS.hyphenationEnabled != 0) ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
      return std::string(tr(STR_DEFAULT_VALUE)) + " (" + defaultEffective + ")";
    }
    if (item.nameId == StrId::STR_GUIDE_DOTS && pendingGuideDotsOverride < 0) {
      const auto defaultEffective = (SETTINGS.guideDots != 0) ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
      return std::string(tr(STR_DEFAULT_VALUE)) + " (" + defaultEffective + ")";
    }
    if (item.nameId == StrId::STR_INLINE_FOOTNOTE_PREVIEWS && pendingInlineFootnotePreviewsOverride < 0) {
      const auto defaultEffective = (SETTINGS.inlineFootnotePreviews != 0) ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
      return std::string(tr(STR_DEFAULT_VALUE)) + " (" + defaultEffective + ")";
    }
    if (item.nameId == StrId::STR_FONT_SIZE_NORMALIZATION && pendingFontSizeNormalizationOverride < 0) {
      const auto defaultEffective = (SETTINGS.fontSizeNormalization != 0) ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
      return std::string(tr(STR_DEFAULT_VALUE)) + " (" + defaultEffective + ")";
    }
  }

  return item.getDisplayValue();
}

void EpubReaderMenuActivity::onExit() { TabbedUiListActivity::onExit(); }

void EpubReaderMenuActivity::activateIndex(const int index) {
  if (index < 0 || index >= listCount()) return;
  auto& item = activeMenuItems()[index];
  if (item.usesSelectorActivity) {
    auto selector = createSelectorActivity(item, renderer, mappedInput);
    if (selector) startActivityForResult(std::move(selector), [this](const ActivityResult&) { requestUpdate(); });
    return;
  }
  if (item.type == SettingType::ACTION) {
    onActionSelected(index);
    return;
  }
  item.toggleValue();
  onSettingToggled(index);
  requestUpdate();
}

void EpubReaderMenuActivity::materializeListWindow() {
  const auto& items = activeMenuItems();
  const int count = static_cast<int>(items.size());
  windowFirst = static_cast<uint16_t>(std::max(0, std::min(activeNav().top, count)));
  windowCount = static_cast<uint16_t>(
      std::min(static_cast<size_t>(count - windowFirst), static_cast<size_t>(LIST_WINDOW_CAPACITY)));
  for (uint16_t offset = 0; offset < windowCount; ++offset) {
    const size_t index = windowFirst + offset;
    windowLabels[offset] = items[index].getTitle();
    windowValues[offset] = getItemValueString(static_cast<int>(index));
    auto& row = windowItems[offset];
    row = {};
    row.label = windowLabels[offset].c_str();
    row.value = windowValues[offset].empty() ? nullptr : windowValues[offset].c_str();
    row.actionValue = static_cast<int16_t>(index);
  }
}

#if READER_MENU_TAB_ICONS
bool EpubReaderMenuActivity::paintTabIcon(fui::DrawTarget& target, const fui::Rect rect, const fui::TabItem& tab,
                                          const uint8_t, void* user) {
  auto* self = static_cast<EpubReaderMenuActivity*>(user);
  const auto ink =
      fui::Paint::solid(tab.selected && self->activeNav().selected < 0 ? fui::Color::White : fui::Color::Black);
  const int16_t left = rect.x;
  const int16_t top = rect.y;
  const int16_t right = static_cast<int16_t>(rect.right() - 1);
  const int16_t bottom = static_cast<int16_t>(rect.bottom() - 1);
  const int16_t centerX = static_cast<int16_t>(rect.x + rect.width / 2);
  const int16_t centerY = static_cast<int16_t>(rect.y + rect.height / 2);
  switch (self->visibleTabs[tab.value]) {
    case MenuTab::Navigation:
      target.line({left, static_cast<int16_t>(top + 3)}, {right, static_cast<int16_t>(top + 3)}, 2, ink);
      target.line({left, centerY}, {right, centerY}, 2, ink);
      target.line({left, static_cast<int16_t>(bottom - 2)}, {right, static_cast<int16_t>(bottom - 2)}, 2, ink);
      break;
    case MenuTab::Settings:
      target.line({left, static_cast<int16_t>(top + 3)}, {right, static_cast<int16_t>(top + 3)}, 1, ink);
      target.line({left, centerY}, {right, centerY}, 1, ink);
      target.line({left, static_cast<int16_t>(bottom - 2)}, {right, static_cast<int16_t>(bottom - 2)}, 1, ink);
      target.fill({static_cast<int16_t>(left + 3), top, 3, 7}, ink);
      target.fill({static_cast<int16_t>(right - 5), static_cast<int16_t>(centerY - 3), 3, 7}, ink);
      target.fill({static_cast<int16_t>(centerX - 1), static_cast<int16_t>(bottom - 5), 3, 7}, ink);
      break;
    case MenuTab::Sync:
      target.line({static_cast<int16_t>(left + 2), static_cast<int16_t>(top + 4)},
                  {static_cast<int16_t>(right - 2), static_cast<int16_t>(top + 4)}, 2, ink);
      target.triangle({right, static_cast<int16_t>(top + 4)}, {static_cast<int16_t>(right - 5), top},
                      {static_cast<int16_t>(right - 5), static_cast<int16_t>(top + 8)}, ink);
      target.line({static_cast<int16_t>(right - 2), static_cast<int16_t>(bottom - 3)},
                  {static_cast<int16_t>(left + 2), static_cast<int16_t>(bottom - 3)}, 2, ink);
      target.triangle({left, static_cast<int16_t>(bottom - 3)}, {static_cast<int16_t>(left + 5), bottom},
                      {static_cast<int16_t>(left + 5), static_cast<int16_t>(bottom - 7)}, ink);
      break;
    case MenuTab::Tools:
      target.stroke({static_cast<int16_t>(left + 1), static_cast<int16_t>(top + 1), 6, 6}, ink, 2, 3);
      target.line({static_cast<int16_t>(left + 6), static_cast<int16_t>(top + 6)},
                  {static_cast<int16_t>(right - 2), static_cast<int16_t>(bottom - 2)}, 3, ink);
      target.stroke({static_cast<int16_t>(right - 5), static_cast<int16_t>(bottom - 5), 5, 5}, ink, 1, 2);
      break;
    case MenuTab::Count:
      break;
  }
  return true;
}
#endif

const char* EpubReaderMenuActivity::tabLabel(const int slot) const {
  static constexpr StrId tabLabels[MENU_TAB_COUNT] = {StrId::STR_READER_NAVIGATION, StrId::STR_SETTINGS_TITLE,
                                                      StrId::STR_SYNC, StrId::STR_READER_TOOLS};
  return I18N.get(tabLabels[static_cast<size_t>(visibleTabs[slot])]);
}

#if READER_MENU_TAB_ICONS
// Adapted from CrossInk's icon-tab reader menu at commit 60cc4da5 (MIT):
// https://github.com/uxjulia/crossink -- the icon-above-label tab design and the per-tab glyphs
// paintTabIcon() draws. The shared tab composition and touch routing this builds on moved to
// TabbedUiListActivity, which carries the rest of that credit.
void EpubReaderMenuActivity::customizeTabBar(UiScreen& screen, fui::TabBarProps& props) {
  // This menu labels its tabs with an icon above the word, so it wants the smaller text and an
  // icon painter; the settings screen takes the plain text default.
  props.text = screen.theme().smallText;
  props.iconSize = 16;
  props.iconPainter = &EpubReaderMenuActivity::paintTabIcon;
  props.iconPainterUserData = this;
}
#endif

void EpubReaderMenuActivity::buildScreen(UiScreen& screen) {
  const Rect contentRect = listContentRect();
  const int16_t top = static_cast<int16_t>(contentRect.y + 75);
  screen.setContentMarginFromScreen(
      fui::Insets{top, static_cast<int16_t>(renderer.getScreenWidth() - (contentRect.x + contentRect.width)),
                  static_cast<int16_t>(renderer.getScreenHeight() - (contentRect.y + contentRect.height)),
                  static_cast<int16_t>(contentRect.x)});

  buildTabBar(screen);
  screen.spacer(6);

  auto props = listProps(screen);
  props.count = static_cast<uint16_t>(listCount());
  syncListViewport(screen, props);
  materializeListWindow();
  props.items = windowItems.data();
  props.itemsWindowFirst = windowFirst;
  props.itemsWindowCount = windowCount;
  noteListSelection(props);
  screen.list(props);
}

void EpubReaderMenuActivity::drawChrome() {
  const Rect contentRect = listContentRect();
  const std::string truncTitle =
      renderer.truncatedText(UI_12_FONT_ID, title.c_str(), contentRect.width - 40, EpdFontFamily::BOLD);
  const int titleX =
      contentRect.x +
      (contentRect.width - renderer.getTextWidth(UI_12_FONT_ID, truncTitle.c_str(), EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentRect.y, truncTitle.c_str(), true, EpdFontFamily::BOLD);

  std::string progressLine;
  if (totalPages > 0) {
    progressLine = std::string(tr(STR_CHAPTER_PREFIX)) + std::to_string(currentPage) + "/" +
                   std::to_string(totalPages) + std::string(tr(STR_PAGES_SEPARATOR));
  }
  progressLine += std::string(tr(STR_BOOK_PREFIX)) + std::to_string(bookProgressPercent) + "%";
  // Centred in the content rect, as the title is, rather than on the screen.
  const int progressX =
      contentRect.x + (contentRect.width - renderer.getTextWidth(UI_10_FONT_ID, progressLine.c_str())) / 2;
  renderer.drawText(UI_10_FONT_ID, progressX, 45 + contentRect.y, progressLine.c_str());
}

const char* EpubReaderMenuActivity::footerConfirmLabel() const {
  // Confirm means "next tab" while the bar holds focus and "select" on a row, so the hint names
  // the tab it would move to, as the settings screen does.
  return selectedPosition() == 0 ? tabLabel((selectedTab() + 1) % tabCount()) : tr(STR_SELECT);
}
