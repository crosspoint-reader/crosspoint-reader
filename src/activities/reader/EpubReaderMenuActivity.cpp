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
                                               const uint8_t initialTextDarkness)
    : Activity("EpubReaderMenu", renderer, mappedInput),
      menuItems(buildMenuItems(hasFootnotes)),
      title(title),
      pendingOrientation(currentOrientation),
      pendingEmbeddedStyleOverride(initialEmbeddedStyleOverride),
      pendingImageRenderingOverride(initialImageRenderingOverride),
      pendingTextDarkness(initialTextDarkness),
      currentPage(currentPage),
      totalPages(totalPages),
      bookProgressPercent(bookProgressPercent) {}

std::vector<EpubReaderMenuActivity::MenuItem> EpubReaderMenuActivity::buildMenuItems(bool hasFootnotes) {
  std::vector<MenuItem> items;
  items.reserve(18);
  // Navigation
  items.push_back({MenuAction::NONE, StrId::STR_READER_NAVIGATION, true});
  items.push_back({MenuAction::SELECT_CHAPTER, StrId::STR_SELECT_CHAPTER});
  items.push_back({MenuAction::GO_TO_PERCENT, StrId::STR_GO_TO_PERCENT});
  if (hasFootnotes) {
    items.push_back({MenuAction::FOOTNOTES, StrId::STR_FOOTNOTES});
  }
  items.push_back({MenuAction::AUTO_PAGE_TURN, StrId::STR_AUTO_TURN_PAGES_PER_MIN});

  // Appearance
  items.push_back({MenuAction::NONE, StrId::STR_READER_APPEARANCE, true});
  items.push_back({MenuAction::EMBEDDED_STYLE, StrId::STR_EMBEDDED_STYLE});
  items.push_back({MenuAction::IMAGE_RENDERING, StrId::STR_IMAGES});
  items.push_back({MenuAction::TEXT_DARKNESS, StrId::STR_TEXT_DARKNESS});
  items.push_back({MenuAction::ROTATE_SCREEN, StrId::STR_ORIENTATION});

  // Synchronisation (only if credentials are set, to avoid confusion)
  if (KOREADER_STORE.hasCredentials()) {
    items.push_back({MenuAction::NONE, StrId::STR_KOREADER_SYNC, true});
    items.push_back({MenuAction::PULL_REMOTE, StrId::STR_PULL_PROGRESS_FROM_OTHER_DEVICES});
    items.push_back({MenuAction::PUSH_LOCAL, StrId::STR_PUSH_PROGRESS_FROM_THIS_DEVICE});
  }

  // Tools
  items.push_back({MenuAction::NONE, StrId::STR_READER_TOOLS, true});
  items.push_back({MenuAction::SCREENSHOT, StrId::STR_SCREENSHOT_BUTTON});
  items.push_back({MenuAction::DISPLAY_QR, StrId::STR_DISPLAY_QR});
  items.push_back({MenuAction::DELETE_CACHE, StrId::STR_DELETE_CACHE});
  items.push_back({MenuAction::GO_HOME, StrId::STR_GO_HOME_BUTTON});
  return items;
}

std::function<bool(int)> EpubReaderMenuActivity::buildSelectablePredicate() const {
  return [this](int index) {
    return index >= 0 && index < static_cast<int>(menuItems.size()) && !menuItems[index].isSeparator;
  };
}

void EpubReaderMenuActivity::onEnter() {
  Activity::onEnter();
  const auto selectablePredicate = buildSelectablePredicate();
  buttonNavigator.setSelectablePredicate(selectablePredicate, static_cast<int>(menuItems.size()));
  if (!selectablePredicate(selectedIndex)) {
    selectedIndex = buttonNavigator.nextIndex(selectedIndex);
  }
  requestUpdate();
}

void EpubReaderMenuActivity::onExit() { Activity::onExit(); }

void EpubReaderMenuActivity::loop() {
  // Handle navigation
  buttonNavigator.onNext([this] {
    selectedIndex = buttonNavigator.nextIndex(selectedIndex);
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = buttonNavigator.previousIndex(selectedIndex);
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const auto selectedAction = menuItems[selectedIndex].action;
    if (selectedAction == MenuAction::NONE) {
      return;
    }
    if (selectedAction == MenuAction::ROTATE_SCREEN) {
      // Cycle orientation preview locally; actual rotation happens on menu exit.
      pendingOrientation = (pendingOrientation + 1) % orientationLabels.size();
      requestUpdate();
      return;
    }

    if (selectedAction == MenuAction::AUTO_PAGE_TURN) {
      selectedPageTurnOption = (selectedPageTurnOption + 1) % pageTurnLabels.size();
      requestUpdate();
      return;
    }

    if (selectedAction == MenuAction::EMBEDDED_STYLE) {
      // Cycle per-book override: default -> ON -> OFF -> default.
      if (pendingEmbeddedStyleOverride < 0) {
        pendingEmbeddedStyleOverride = 1;
      } else if (pendingEmbeddedStyleOverride > 0) {
        pendingEmbeddedStyleOverride = 0;
      } else {
        pendingEmbeddedStyleOverride = -1;
      }
      requestUpdate();
      return;
    }

    if (selectedAction == MenuAction::IMAGE_RENDERING) {
      // Cycle per-book override: default -> display -> placeholder -> suppress -> default.
      if (pendingImageRenderingOverride < 0) {
        pendingImageRenderingOverride = 0;
      } else if (pendingImageRenderingOverride >= 2) {
        pendingImageRenderingOverride = -1;
      } else {
        pendingImageRenderingOverride++;
      }
      requestUpdate();
      return;
    }

    if (selectedAction == MenuAction::TEXT_DARKNESS) {
      pendingTextDarkness = (pendingTextDarkness + 1) % textDarknessLabels.size();
      requestUpdate();
      return;
    }

    setResult(MenuResult{static_cast<int>(selectedAction), pendingOrientation, selectedPageTurnOption,
                         pendingEmbeddedStyleOverride, pendingImageRenderingOverride, pendingTextDarkness});
    finish();
    return;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
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
    return;
  }
}

void EpubReaderMenuActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const Rect contentRect = UITheme::getContentRect(renderer, true, false);

  // Title
  const std::string truncTitle =
      renderer.truncatedText(UI_12_FONT_ID, title.c_str(), contentRect.width - 40, EpdFontFamily::BOLD);
  // Manual centering so we can respect the content gutter.
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

  GUI.drawList(
      renderer, Rect{contentRect.x, startY, contentRect.width, listHeight}, static_cast<int>(menuItems.size()),
      selectedIndex,
      [this](int index) {
        const auto& item = menuItems[index];
        const auto title = I18N.get(item.labelId);
        return item.isSeparator ? UITheme::makeSeparatorTitle(title) : title;
      },
      nullptr, nullptr,
      [this](int index) {
        const auto& item = menuItems[index];
        switch (item.action) {
          case MenuAction::ROTATE_SCREEN:
            return std::string(I18N.get(orientationLabels[pendingOrientation]));
          case MenuAction::AUTO_PAGE_TURN:
            return std::string(pageTurnLabels[selectedPageTurnOption]);
          case MenuAction::EMBEDDED_STYLE:
            if (pendingEmbeddedStyleOverride == 1) {
              return std::string(tr(STR_STATE_ON));
            } else if (pendingEmbeddedStyleOverride == 0) {
              return std::string(tr(STR_STATE_OFF));
            }
            return std::string(tr(STR_DEFAULT_VALUE));
          case MenuAction::IMAGE_RENDERING:
            if (pendingImageRenderingOverride >= 0 && pendingImageRenderingOverride < imageRenderingLabels.size()) {
              return std::string(I18N.get(imageRenderingLabels[pendingImageRenderingOverride]));
            }
            return std::string(tr(STR_DEFAULT_VALUE));
          case MenuAction::TEXT_DARKNESS: {
            const uint8_t idx = (pendingTextDarkness < textDarknessLabels.size()) ? pendingTextDarkness : 0;
            return std::string(I18N.get(textDarknessLabels[idx]));
          }
          default:
            return std::string();
        }
      },
      true);

  // Footer / Hints
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
