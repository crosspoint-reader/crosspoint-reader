#include "EpubReaderMenuActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "SdCardFontGlobals.h"
#include "activities/settings/SettingsSubmenuActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
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
  // Built-in: enumValues[0] is STR_DEFAULT_VALUE, [1..] are built-in families
  // in CrossPointSettings::FONT_FAMILY order.
  const auto idx = static_cast<size_t>(SETTINGS.fontFamily + 1);
  if (idx < item.enumValues.size()) {
    return I18N.get(item.enumValues[idx]);
  }
  return {};
}
}  // namespace

EpubReaderMenuActivity::EpubReaderMenuActivity(
    GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& title, const int currentPage,
    const int totalPages, const int bookProgressPercent, const uint8_t currentOrientation, const bool hasFootnotes,
    const int8_t initialEmbeddedStyleOverride, const int8_t initialImageRenderingOverride,
    const int8_t initialFontFamilyOverride, const std::string& initialSdFontFamilyOverride,
    const int8_t initialFontSizeOverride, const uint8_t initialTextDarkness, const bool initialBionicReadingOverride,
    const int8_t initialParagraphAlignmentOverride, const bool hasStarredPages, const bool isCurrentPageStarred,
    const bool hasPrintedPages)
    : MenuListActivity("EpubReaderMenu", renderer, mappedInput),
      currentPageStarred(isCurrentPageStarred),
      pendingOrientation(currentOrientation),
      pendingEmbeddedStyleOverride(initialEmbeddedStyleOverride),
      pendingImageRenderingOverride(initialImageRenderingOverride),
      pendingFontFamilyOverride(initialFontFamilyOverride),
      pendingSdFontFamilyOverride(initialSdFontFamilyOverride),
      pendingFontSizeOverride(initialFontSizeOverride),
      pendingTextDarkness(initialTextDarkness),
      pendingBionicReading(initialBionicReadingOverride),
      pendingParagraphAlignmentOverride(initialParagraphAlignmentOverride),
      title(title),
      currentPage(currentPage),
      totalPages(totalPages),
      bookProgressPercent(bookProgressPercent) {
  buildMenuItems(hasFootnotes, hasStarredPages, hasPrintedPages);
}

void EpubReaderMenuActivity::buildMenuItems(bool hasFootnotes, bool hasStarredPages, bool hasPrintedPages) {
  menuItems.reserve(20);

  // --- Navigation ---
  menuItems.push_back(SettingInfo::Separator(StrId::STR_READER_NAVIGATION));
  menuItems.push_back(SettingInfo::Action(StrId::STR_SELECT_CHAPTER, SettingAction::None));
  menuItems.push_back(SettingInfo::Action(StrId::STR_GO_TO_PERCENT, SettingAction::None));
  if (hasPrintedPages) {
    menuItems.push_back(SettingInfo::Action(StrId::STR_GO_TO_PRINTED_PAGE, SettingAction::None));
  }

  // Bookmarks, footnotes
  menuItems.push_back(SettingInfo::Separator(StrId::STR_READER_BOOKMARKS));

  menuItems.push_back(SettingInfo::Action(StrId::STR_STAR_PAGE, SettingAction::None));
  if (hasStarredPages) {
    menuItems.push_back(SettingInfo::Action(StrId::STR_STARRED_PAGES, SettingAction::None));
  }
  if (hasFootnotes) {
    menuItems.push_back(SettingInfo::Action(StrId::STR_FOOTNOTES, SettingAction::None));
  }

  // --- Appearance ---
  menuItems.push_back(SettingInfo::Separator(StrId::STR_READER_APPEARANCE));

  auto* self = this;
  // Orientation: straightforward 0-3 cycle
  menuItems.push_back(SettingInfo::DynamicEnumCtx(
      StrId::STR_ORIENTATION,
      {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_INVERTED, StrId::STR_LANDSCAPE_CCW}, self,
      [](const void* ctx) -> uint8_t { return static_cast<const EpubReaderMenuActivity*>(ctx)->pendingOrientation; },
      [](void* ctx, uint8_t v) { static_cast<EpubReaderMenuActivity*>(ctx)->pendingOrientation = v; }));

  // Embedded style: cycles default(-1) -> ON(1) -> OFF(0) via DynamicEnum indices 0/1/2
  menuItems.push_back(SettingInfo::DynamicEnumCtx(
                          StrId::STR_EMBEDDED_STYLE,
                          {StrId::STR_DEFAULT_VALUE, StrId::STR_STATE_ON, StrId::STR_STATE_OFF}, self,
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
                          })
                          .withSubmenu(StrId::STR_READER_OVERRIDES));

  // Image rendering: cycles default(-1) -> display(0) -> placeholder(1) -> suppress(2)
  menuItems.push_back(SettingInfo::DynamicEnumCtx(
                          StrId::STR_IMAGES,
                          {StrId::STR_DEFAULT_VALUE, StrId::STR_IMAGES_DISPLAY, StrId::STR_IMAGES_PLACEHOLDER,
                           StrId::STR_IMAGES_SUPPRESS},
                          self,
                          [](const void* ctx) -> uint8_t {
                            const auto* s = static_cast<const EpubReaderMenuActivity*>(ctx);
                            return (s->pendingImageRenderingOverride < 0) ? 0 : (s->pendingImageRenderingOverride + 1);
                          },
                          [](void* ctx, uint8_t v) {
                            auto* s = static_cast<EpubReaderMenuActivity*>(ctx);
                            s->pendingImageRenderingOverride = (v == 0) ? -1 : static_cast<int8_t>(v - 1);
                          })
                          .withSubmenu(StrId::STR_READER_OVERRIDES));

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
                             .withSubmenu(StrId::STR_READER_OVERRIDES);

    familySetting.enumLabels = {tr(STR_DEFAULT_VALUE), tr(STR_BOOKERLY), tr(STR_NOTO_SANS)};
    for (const auto& fam : families) {
      familySetting.enumLabels.push_back(fam.name);
    }
    menuItems.push_back(std::move(familySetting));
  }

  // Reader font size: cycles default(-1) -> Small(0) -> Medium(1) -> Large(2) -> X Large(3) -> Tiny(4)
  menuItems.push_back(SettingInfo::DynamicEnumCtx(
                          StrId::STR_FONT_SIZE,
                          {StrId::STR_DEFAULT_VALUE, StrId::STR_SMALL, StrId::STR_MEDIUM, StrId::STR_LARGE,
                           StrId::STR_X_LARGE, StrId::STR_TINY},
                          self,
                          [](const void* ctx) -> uint8_t {
                            const auto* s = static_cast<const EpubReaderMenuActivity*>(ctx);
                            return (s->pendingFontSizeOverride < 0)
                                       ? 0
                                       : static_cast<uint8_t>(s->pendingFontSizeOverride + 1);
                          },
                          [](void* ctx, uint8_t v) {
                            auto* s = static_cast<EpubReaderMenuActivity*>(ctx);
                            s->pendingFontSizeOverride = (v == 0) ? -1 : static_cast<int8_t>(v - 1);
                          })
                          .withSubmenu(StrId::STR_READER_OVERRIDES));

  // Text darkness: straightforward 0-3 cycle
  menuItems.push_back(
      SettingInfo::DynamicEnumCtx(
          StrId::STR_TEXT_DARKNESS, {StrId::STR_NORMAL, StrId::STR_DARK, StrId::STR_EXTRA_DARK, StrId::STR_MAX_DARK},
          self,
          [](const void* ctx) -> uint8_t {
            return static_cast<const EpubReaderMenuActivity*>(ctx)->pendingTextDarkness;
          },
          [](void* ctx, uint8_t v) { static_cast<EpubReaderMenuActivity*>(ctx)->pendingTextDarkness = v; })
          .withSubmenu(StrId::STR_READER_OVERRIDES));

  menuItems.push_back(
      SettingInfo::DynamicEnumCtx(
          StrId::STR_BIONIC_READING, {StrId::STR_STATE_OFF, StrId::STR_STATE_ON}, self,
          [](const void* ctx) -> uint8_t {
            return static_cast<const EpubReaderMenuActivity*>(ctx)->pendingBionicReading ? 1 : 0;
          },
          [](void* ctx, uint8_t v) { static_cast<EpubReaderMenuActivity*>(ctx)->pendingBionicReading = (v != 0); })
          .withSubmenu(StrId::STR_READER_OVERRIDES));

  // Paragraph alignment: default(-1) + the 5 global options
  menuItems.push_back(SettingInfo::DynamicEnumCtx(
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
                          .withSubmenu(StrId::STR_READER_OVERRIDES));

  // Helper functions, reading ruler, auto page turn, orientation
  menuItems.push_back(SettingInfo::Separator(StrId::STR_READER_UTILS));
  // Auto page turn: ACTION type with custom cycling in onActionSelected
  menuItems.push_back(SettingInfo::Action(StrId::STR_AUTO_TURN_PAGES_PER_MIN, SettingAction::None));

  // --- Synchronisation (only if credentials are set) ---
  if (KOREADER_STORE.hasCredentials()) {
    menuItems.push_back(SettingInfo::Separator(StrId::STR_KOREADER_SYNC));
    menuItems.push_back(SettingInfo::Action(StrId::STR_PULL_PROGRESS_FROM_OTHER_DEVICES, SettingAction::None));
    menuItems.push_back(SettingInfo::Action(StrId::STR_PUSH_PROGRESS_FROM_THIS_DEVICE, SettingAction::None));
  }

  // --- Tools ---
  menuItems.push_back(SettingInfo::Separator(StrId::STR_READER_TOOLS));
  menuItems.push_back(SettingInfo::Action(StrId::STR_READING_STATS_FOR_THIS_BOOK, SettingAction::None)
                          .withSubmenu(StrId::STR_READER_TOOLS));
  menuItems.push_back(
      SettingInfo::Action(StrId::STR_MARK_AS_READ, SettingAction::None).withSubmenu(StrId::STR_READER_TOOLS));
  menuItems.push_back(
      SettingInfo::Action(StrId::STR_DELETE_CACHE, SettingAction::None).withSubmenu(StrId::STR_READER_TOOLS));
  menuItems.push_back(SettingInfo::Separator(StrId::STR_TOOL_UTILITIES).withSubmenu(StrId::STR_READER_TOOLS));
  menuItems.push_back(
      SettingInfo::Action(StrId::STR_SCREENSHOT_BUTTON, SettingAction::None).withSubmenu(StrId::STR_READER_TOOLS));
  menuItems.push_back(
      SettingInfo::Action(StrId::STR_DISPLAY_QR, SettingAction::None).withSubmenu(StrId::STR_READER_TOOLS));
  menuItems.push_back(SettingInfo::Action(StrId::STR_GO_HOME_BUTTON, SettingAction::None));
#ifdef ENABLE_BENCHMARKS
  menuItems.push_back(SettingInfo::Separator(StrId::STR_NONE_OPT).withSubmenu(StrId::STR_READER_TOOLS));
  menuItems.push_back(
      SettingInfo::Action(StrId::STR_RENDER_BENCHMARK, SettingAction::None).withSubmenu(StrId::STR_READER_TOOLS));
#endif
}

EpubReaderMenuActivity::MenuAction EpubReaderMenuActivity::actionForNameId(StrId nameId) {
  switch (nameId) {
    case StrId::STR_SELECT_CHAPTER:
      return MenuAction::SELECT_CHAPTER;
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
      return MenuAction::READING_STATS;
    default:
      return MenuAction::NONE;
  }
}

EpubReaderMenuActivity::MenuAction EpubReaderMenuActivity::actionForSettingAction(SettingAction action) {
  switch (action) {
    case SettingAction::None:
    case SettingAction::Submenu:
      return MenuAction::NONE;
    default:
      return MenuAction::NONE;
  }
}

void EpubReaderMenuActivity::finishWithAction(MenuAction action) {
  setResult(MenuResult{static_cast<int>(action), -1, pendingOrientation, selectedPageTurnOption,
                       pendingEmbeddedStyleOverride, pendingImageRenderingOverride, pendingFontFamilyOverride,
                       pendingSdFontFamilyOverride, pendingFontSizeOverride, pendingTextDarkness,
                       static_cast<uint8_t>(pendingBionicReading), pendingParagraphAlignmentOverride});
  finish();
}

void EpubReaderMenuActivity::onActionSelected(int index) {
  const auto& item = menuItems[index];

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
  result.data = MenuResult{-1,
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
                           pendingParagraphAlignmentOverride};
  setResult(std::move(result));
  finish();
}

std::string EpubReaderMenuActivity::getItemValueString(int index) const {
  const auto& item = menuItems[index];

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
    if (item.action == SettingAction::Submenu) {
      return MenuListActivity::getItemValueString(index);
    }
    return {};
  }

  if (item.type == SettingType::ENUM) {
    if (item.nameId == StrId::STR_EMBEDDED_STYLE && pendingEmbeddedStyleOverride < 0) {
      const auto defaultEffective = (SETTINGS.embeddedStyle != 0) ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
      return std::string(tr(STR_DEFAULT_VALUE)) + " (" + defaultEffective + ")";
    }
    if (item.nameId == StrId::STR_IMAGES && pendingImageRenderingOverride < 0) {
      const auto defaultIndex = static_cast<size_t>(SETTINGS.imageRendering + 1);
      if (defaultIndex < item.enumValues.size()) {
        return std::string(tr(STR_DEFAULT_VALUE)) + " (" + I18N.get(item.enumValues[defaultIndex]) + ")";
      }
    }
    if (item.nameId == StrId::STR_FONT_FAMILY && pendingFontFamilyOverride < 0 && pendingSdFontFamilyOverride.empty()) {
      const auto label = defaultFontFamilyLabel(item);
      if (!label.empty()) {
        return std::string(tr(STR_DEFAULT_VALUE)) + " (" + label + ")";
      }
    }
    if (item.nameId == StrId::STR_FONT_SIZE && pendingFontSizeOverride < 0) {
      const auto defaultIndex = static_cast<size_t>(SETTINGS.fontSize + 1);
      if (defaultIndex < item.enumValues.size()) {
        return std::string(tr(STR_DEFAULT_VALUE)) + " (" + I18N.get(item.enumValues[defaultIndex]) + ")";
      }
    }
    if (item.nameId == StrId::STR_PARA_ALIGNMENT && pendingParagraphAlignmentOverride < 0) {
      const auto defaultIndex = static_cast<size_t>(SETTINGS.paragraphAlignment + 1);
      if (defaultIndex < item.enumValues.size()) {
        return std::string(tr(STR_DEFAULT_VALUE)) + " (" + I18N.get(item.enumValues[defaultIndex]) + ")";
      }
    }
  }

  // DynamicEnum items use the standard display
  return MenuListActivity::getItemValueString(index);
}

void EpubReaderMenuActivity::openSubmenu(const SettingInfo& submenuEntry) {
  auto it = std::find_if(submenuData.begin(), submenuData.end(),
                         [&submenuEntry](const SettingInfo::SubmenuData& d) { return d.id == submenuEntry.nameId; });
  if (it == submenuData.end()) return;

  auto itemValueStringOverride = [this](const SettingInfo& item) -> std::string {
    if (item.nameId == StrId::STR_EMBEDDED_STYLE && pendingEmbeddedStyleOverride < 0) {
      const auto defaultEffective = (SETTINGS.embeddedStyle != 0) ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
      return std::string(tr(STR_DEFAULT_VALUE)) + " (" + defaultEffective + ")";
    }
    if (item.nameId == StrId::STR_IMAGES && pendingImageRenderingOverride < 0) {
      const auto valueIndex = static_cast<size_t>(SETTINGS.imageRendering + 1);
      if (valueIndex < item.enumValues.size()) {
        return std::string(tr(STR_DEFAULT_VALUE)) + " (" + I18N.get(item.enumValues[valueIndex]) + ")";
      }
    }
    if (item.nameId == StrId::STR_FONT_FAMILY && pendingFontFamilyOverride < 0 && pendingSdFontFamilyOverride.empty()) {
      const auto label = defaultFontFamilyLabel(item);
      if (!label.empty()) {
        return std::string(tr(STR_DEFAULT_VALUE)) + " (" + label + ")";
      }
    }
    if (item.nameId == StrId::STR_FONT_SIZE && pendingFontSizeOverride < 0) {
      const auto valueIndex = static_cast<size_t>(SETTINGS.fontSize + 1);
      if (valueIndex < item.enumValues.size()) {
        return std::string(tr(STR_DEFAULT_VALUE)) + " (" + I18N.get(item.enumValues[valueIndex]) + ")";
      }
    }
    if (item.nameId == StrId::STR_PARA_ALIGNMENT && pendingParagraphAlignmentOverride < 0) {
      const auto valueIndex = static_cast<size_t>(SETTINGS.paragraphAlignment + 1);
      if (valueIndex < item.enumValues.size()) {
        return std::string(tr(STR_DEFAULT_VALUE)) + " (" + I18N.get(item.enumValues[valueIndex]) + ")";
      }
    }
    return item.getDisplayValue();
  };

  startActivityForResult(std::make_unique<SettingsSubmenuActivity>(renderer, mappedInput, submenuEntry.nameId,
                                                                   it->items, std::move(itemValueStringOverride)),
                         [this](const ActivityResult& result) {
                           if (!result.isCancelled) {
                             const auto* menuResult = std::get_if<MenuResult>(&result.data);
                             if (menuResult) {
                               if (menuResult->action != -1) {
                                 const auto action =
                                     actionForSettingAction(static_cast<SettingAction>(menuResult->action));
                                 if (action != MenuAction::NONE) {
                                   finishWithAction(action);
                                   return;
                                 }
                               }
                               if (menuResult->nameId != -1) {
                                 const auto action = actionForNameId(static_cast<StrId>(menuResult->nameId));
                                 if (action != MenuAction::NONE) {
                                   finishWithAction(action);
                                   return;
                                 }
                               }
                             }
                           }
                           requestUpdate();
                         });
}

void EpubReaderMenuActivity::toggleCurrentItem() {
  if (selectedIndex < 0 || selectedIndex >= static_cast<int>(menuItems.size())) return;
  const auto& item = menuItems[selectedIndex];
  if (item.isSeparator) return;

  if (item.type == SettingType::ACTION) {
    if (item.action == SettingAction::Submenu) {
      openSubmenu(item);
      return;
    }
    onActionSelected(selectedIndex);
    return;
  }

  menuItems[selectedIndex].toggleValue();
  onSettingToggled(selectedIndex);
  requestUpdate();
}

void EpubReaderMenuActivity::onEnter() { MenuListActivity::onEnter(); }

void EpubReaderMenuActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);

  // Title
  const std::string truncTitle =
      renderer.truncatedText(UI_12_FONT_ID, title.c_str(), contentRect.width - 40, EpdFontFamily::BOLD);
  const int titleX =
      contentRect.x +
      (contentRect.width - renderer.getTextWidth(UI_12_FONT_ID, truncTitle.c_str(), EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentRect.y, truncTitle.c_str(), true, EpdFontFamily::BOLD);

  // Progress summary
  std::string progressLine;
  if (totalPages > 0) {
    progressLine = std::string(tr(STR_CHAPTER_PREFIX)) + std::to_string(currentPage) + "/" +
                   std::to_string(totalPages) + std::string(tr(STR_PAGES_SEPARATOR));
  }
  progressLine += std::string(tr(STR_BOOK_PREFIX)) + std::to_string(bookProgressPercent) + "%";
  renderer.drawCenteredText(UI_10_FONT_ID, 45, progressLine.c_str());

  // Menu Items
  const int startY = 75 + contentRect.y;
  const int listHeight = contentRect.height - (startY - contentRect.y);
  drawMenuList(Rect{contentRect.x, startY, contentRect.width, listHeight});

  // Footer / Hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
