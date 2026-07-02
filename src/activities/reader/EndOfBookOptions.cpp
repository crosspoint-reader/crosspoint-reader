#include "EndOfBookOptions.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/NextBookFinder.h"

namespace {
// Vertical layout of the end screen. The plain title position matches the historical
// end-of-book screen; the Ask layout starts higher so the list fits in landscape too.
constexpr int TITLE_Y = 300;
constexpr int ASK_TITLE_Y = 120;
constexpr int ASK_SUBTITLE_Y = 170;
constexpr int ASK_LIST_TOP = 200;
constexpr int NEXT_HINT_Y = 345;

// Display name without the file extension, mirroring the file browser rows
std::string displayName(const std::string& filename) {
  const auto pos = filename.rfind('.');
  return filename.substr(0, pos);
}

// Right-truncate text with an ellipsis so it fits maxWidth
std::string fitText(const GfxRenderer& renderer, const int fontId, std::string text, const int maxWidth) {
  if (renderer.getTextWidth(fontId, text.c_str()) <= maxWidth) {
    return text;
  }
  const char ellipsis[] = "\xe2\x80\xa6";  // UTF-8 ellipsis (…)
  while (!text.empty()) {
    // Drop the last UTF-8 code point, skipping continuation bytes
    size_t i = text.size() - 1;
    while (i > 0 && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) {
      i--;
    }
    text.erase(i);
    if (renderer.getTextWidth(fontId, (text + ellipsis).c_str()) <= maxWidth) {
      break;
    }
  }
  return text + ellipsis;
}
}  // namespace

void EndOfBookOptions::loadOnce(const std::string& currentBookPath) {
  if (isLoaded || SETTINGS.endOfBookBehavior == CrossPointSettings::EOB_HOME) {
    return;
  }
  folder = FsHelpers::extractFolderPath(currentBookPath);
  names = NextBookFinder::findNextBooks(currentBookPath, MAX_SUGGESTIONS);
  selector = 0;
  isLoaded = true;
}

bool EndOfBookOptions::askMenuActive() const {
  return SETTINGS.endOfBookBehavior == CrossPointSettings::EOB_ASK && isLoaded && !names.empty();
}

std::string EndOfBookOptions::firstSuggestionPath() const { return names.empty() ? std::string() : fullPath(0); }

std::string EndOfBookOptions::fullPath(const size_t index) const {
  if (index >= names.size()) {
    return {};
  }
  return folder == "/" ? "/" + names[index] : folder + "/" + names[index];
}

EndOfBookOptions::Action EndOfBookOptions::handleAskInput(const MappedInputManager& input, std::string* openPath) {
  if (input.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selector < static_cast<int>(names.size())) {
      if (openPath) {
        *openPath = fullPath(selector);
      }
      return Action::OpenBook;
    }
    return Action::GoHome;  // "Home" entry selected
  }

  // Short-press Back returns to the last page; a long press falls through to the
  // reader's own handler (file browser).
  if (input.wasReleased(MappedInputManager::Button::Back) && input.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    return Action::LastPage;
  }

  const auto [prev, next, fromTilt] = ReaderUtils::detectPageTurn(input);
  const int itemCount = static_cast<int>(names.size()) + 1;  // + "Home" entry
  if (prev && selector > 0) {
    selector--;
    return Action::Redraw;
  }
  if (next && selector < itemCount - 1) {
    selector++;
    return Action::Redraw;
  }
  return Action::None;
}

void EndOfBookOptions::render(GfxRenderer& renderer, const MappedInputManager& input) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int maxTextWidth = pageWidth - metrics.contentSidePadding * 2;

  if (!askMenuActive()) {
    renderer.drawCenteredText(UI_12_FONT_ID, TITLE_Y, tr(STR_END_OF_BOOK), true, EpdFontFamily::BOLD);
    if (SETTINGS.endOfBookBehavior == CrossPointSettings::EOB_NEXT_BOOK && !names.empty()) {
      const std::string hint = std::string(tr(STR_EOB_NEXT)) + ": " + displayName(names.front());
      renderer.drawCenteredText(UI_10_FONT_ID, NEXT_HINT_Y,
                                fitText(renderer, UI_10_FONT_ID, hint, maxTextWidth).c_str());
    }
    return;
  }

  // Ask mode: title, suggestion list (+ Home entry) and button hints
  renderer.drawCenteredText(UI_12_FONT_ID, ASK_TITLE_Y, tr(STR_END_OF_BOOK), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, ASK_SUBTITLE_Y, tr(STR_EOB_CONTINUE_WITH));

  const int pageHeight = renderer.getScreenHeight();
  const int listHeight = pageHeight - ASK_LIST_TOP - metrics.buttonHintsHeight - metrics.verticalSpacing;
  GUI.drawList(renderer, Rect{0, ASK_LIST_TOP, pageWidth, listHeight}, static_cast<int>(names.size()) + 1, selector,
               [this](const int index) {
                 return index < static_cast<int>(names.size()) ? displayName(names[index])
                                                               : std::string(tr(STR_EOB_HOME));
               });

  const auto labels = input.mapLabels(tr(STR_EOB_LAST_PAGE), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
