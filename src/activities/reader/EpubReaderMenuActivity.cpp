#include "EpubReaderMenuActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

EpubReaderMenuActivity::EpubReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                               const std::string& title, const int currentPage, const int totalPages,
                                               const int bookProgressPercent, const uint8_t currentOrientation,
                                               const bool hasFootnotes, const int8_t initialEmbeddedStyleOverride,
                                               const int8_t initialImageRenderingOverride,
                                               const uint8_t initialTextDarkness, const bool hasStarredPages,
                                               const bool isCurrentPageStarred)
    : MenuListActivity("EpubReaderMenu", renderer, mappedInput),
      currentPageStarred(isCurrentPageStarred),
      pendingOrientation(currentOrientation),
      pendingEmbeddedStyleOverride(initialEmbeddedStyleOverride),
      pendingImageRenderingOverride(initialImageRenderingOverride),
      pendingTextDarkness(initialTextDarkness),
      title(title),
      currentPage(currentPage),
      totalPages(totalPages),
      bookProgressPercent(bookProgressPercent) {
  buildMenuItems(hasFootnotes, hasStarredPages);
}

void EpubReaderMenuActivity::buildMenuItems(bool hasFootnotes, bool hasStarredPages) {
  menuItems.reserve(19);

  // --- Navigation ---
  menuItems.push_back(SettingInfo::Separator(StrId::STR_READER_NAVIGATION));
  menuItems.push_back(SettingInfo::Action(StrId::STR_SELECT_CHAPTER, SettingAction::None));
  menuItems.push_back(SettingInfo::Action(StrId::STR_GO_TO_PERCENT, SettingAction::None));

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

  // Embedded style: cycles default(-1) -> ON(1) -> OFF(0) via DynamicEnum indices 0/1/2
  menuItems.push_back(SettingInfo::DynamicEnumCtx(
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
  menuItems.push_back(SettingInfo::DynamicEnumCtx(
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

  // Text darkness: straightforward 0-3 cycle
  menuItems.push_back(SettingInfo::DynamicEnumCtx(
      StrId::STR_TEXT_DARKNESS, {StrId::STR_NORMAL, StrId::STR_DARK, StrId::STR_EXTRA_DARK, StrId::STR_MAX_DARK}, self,
      [](void* ctx) -> uint8_t { return static_cast<EpubReaderMenuActivity*>(ctx)->pendingTextDarkness; },
      [](void* ctx, uint8_t v) { static_cast<EpubReaderMenuActivity*>(ctx)->pendingTextDarkness = v; }));

  // Helper functions, reading ruler, auto page turn, orientation
  menuItems.push_back(SettingInfo::Separator(StrId::STR_READER_UTILS));
  // Auto page turn: ACTION type with custom cycling in onActionSelected
  menuItems.push_back(SettingInfo::Action(StrId::STR_AUTO_TURN_PAGES_PER_MIN, SettingAction::None));
  // Orientation: straightforward 0-3 cycle
  menuItems.push_back(SettingInfo::DynamicEnumCtx(
      StrId::STR_ORIENTATION,
      {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_INVERTED, StrId::STR_LANDSCAPE_CCW}, self,
      [](void* ctx) -> uint8_t { return static_cast<EpubReaderMenuActivity*>(ctx)->pendingOrientation; },
      [](void* ctx, uint8_t v) { static_cast<EpubReaderMenuActivity*>(ctx)->pendingOrientation = v; }));

  // --- Synchronisation (only if credentials are set) ---
  if (KOREADER_STORE.hasCredentials()) {
    menuItems.push_back(SettingInfo::Separator(StrId::STR_KOREADER_SYNC));
    menuItems.push_back(SettingInfo::Action(StrId::STR_PULL_PROGRESS_FROM_OTHER_DEVICES, SettingAction::None));
    menuItems.push_back(SettingInfo::Action(StrId::STR_PUSH_PROGRESS_FROM_THIS_DEVICE, SettingAction::None));
  }

  // --- Tools ---
  menuItems.push_back(SettingInfo::Separator(StrId::STR_READER_TOOLS));
  menuItems.push_back(SettingInfo::Action(StrId::STR_SCREENSHOT_BUTTON, SettingAction::None));
  menuItems.push_back(SettingInfo::Action(StrId::STR_DISPLAY_QR, SettingAction::None));
  menuItems.push_back(SettingInfo::Action(StrId::STR_DELETE_CACHE, SettingAction::None));
  menuItems.push_back(SettingInfo::Action(StrId::STR_GO_HOME_BUTTON, SettingAction::None));
}

EpubReaderMenuActivity::MenuAction EpubReaderMenuActivity::actionForNameId(StrId nameId) {
  switch (nameId) {
    case StrId::STR_SELECT_CHAPTER:
      return MenuAction::SELECT_CHAPTER;
    case StrId::STR_GO_TO_PERCENT:
      return MenuAction::GO_TO_PERCENT;
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
    case StrId::STR_DELETE_CACHE:
      return MenuAction::DELETE_CACHE;
    case StrId::STR_GO_HOME_BUTTON:
      return MenuAction::GO_HOME;
    default:
      return MenuAction::NONE;
  }
}

void EpubReaderMenuActivity::finishWithAction(MenuAction action) {
  setResult(MenuResult{static_cast<int>(action), pendingOrientation, selectedPageTurnOption,
                       pendingEmbeddedStyleOverride, pendingImageRenderingOverride, pendingTextDarkness});
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
                           pendingOrientation,
                           selectedPageTurnOption,
                           pendingEmbeddedStyleOverride,
                           pendingImageRenderingOverride,
                           pendingTextDarkness};
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

  // Plain ACTION items (select chapter, screenshot, etc.) show no value
  if (item.type == SettingType::ACTION) return {};

  // DynamicEnum items use the standard display
  return MenuListActivity::getItemValueString(index);
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
