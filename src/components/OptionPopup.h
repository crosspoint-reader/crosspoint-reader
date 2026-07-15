#pragma once
#include <I18n.h>

#include <functional>
#include <string>
#include <vector>

#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"

class OptionPopup {
 public:
  void show(StrId titleId, const StrId* optionIds, int optionCount, int currentIndex,
            std::function<void(int)> onSelect, std::function<void(int)> onHighlight = {},
            std::function<void()> onCancel = {}) {
    title = I18N.get(titleId);
    ownedStrings.resize(optionCount);
    for (int i = 0; i < optionCount; i++) {
      ownedStrings[i] = I18N.get(optionIds[i]);
    }
    beginShow(currentIndex, std::move(onSelect), std::move(onHighlight), std::move(onCancel));
  }

  void show(const char* titleStr, const char* const* options, int optionCount, int currentIndex,
            std::function<void(int)> onSelect, std::function<void(int)> onHighlight = {},
            std::function<void()> onCancel = {}) {
    title = titleStr;
    ownedStrings.resize(optionCount);
    for (int i = 0; i < optionCount; i++) {
      ownedStrings[i] = options[i];
    }
    beginShow(currentIndex, std::move(onSelect), std::move(onHighlight), std::move(onCancel));
  }

  void show(StrId titleId, const std::vector<std::string>& options, int currentIndex,
            std::function<void(int)> onSelect, std::function<void(int)> onHighlight = {},
            std::function<void()> onCancel = {}) {
    title = I18N.get(titleId);
    ownedStrings = options;
    beginShow(currentIndex, std::move(onSelect), std::move(onHighlight), std::move(onCancel));
  }

  bool handleInput(MappedInputManager& input, const std::function<void()>& requestUpdate) {
    if (!active) return false;

    const int count = static_cast<int>(ownedStrings.size());
    // Use NavPrevious/NavNext so left/right follow orientation (Orient front buttons).
    if (input.wasPressed(MappedInputManager::Button::NavPrevious)) {
      selectedIndex = (selectedIndex - 1 + count) % count;
      if (onHighlightCallback) {
        onHighlightCallback(selectedIndex);
      }
      requestUpdate();
      return true;
    }
    if (input.wasPressed(MappedInputManager::Button::NavNext)) {
      selectedIndex = (selectedIndex + 1) % count;
      if (onHighlightCallback) {
        onHighlightCallback(selectedIndex);
      }
      requestUpdate();
      return true;
    }
    if (input.wasPressed(MappedInputManager::Button::Confirm)) {
      active = false;
      if (onSelectCallback) {
        onSelectCallback(selectedIndex);
      }
      requestUpdate();
      return true;
    }
    if (input.wasPressed(MappedInputManager::Button::Back)) {
      active = false;
      if (onCancelCallback) {
        onCancelCallback();
      }
      requestUpdate();
      return true;
    }
    return true;
  }

  bool processRender(GfxRenderer& renderer, const MappedInputManager& input) const {
    if (!active) return false;
    // Clear so orientation preview (e.g. Portrait → Landscape) does not leave the prior frame.
    renderer.clearScreen();
    const auto popupLabels = input.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, popupLabels.btn1, popupLabels.btn2, popupLabels.btn3, popupLabels.btn4);
    render(renderer);
    renderer.displayBuffer();
    return true;
  }

  void render(const GfxRenderer& renderer) const {
    if (!active) return;
    GUI.drawOptionPopup(renderer, title.c_str(), ownedStrings, selectedIndex);
  }

  bool isActive() const { return active; }

 private:
  void beginShow(int currentIndex, std::function<void(int)> onSelect, std::function<void(int)> onHighlight,
                 std::function<void()> onCancel) {
    selectedIndex = currentIndex;
    onSelectCallback = std::move(onSelect);
    onHighlightCallback = std::move(onHighlight);
    onCancelCallback = std::move(onCancel);
    active = true;
  }

  bool active = false;
  std::string title;
  std::vector<std::string> ownedStrings;
  int selectedIndex = 0;
  std::function<void(int)> onSelectCallback;
  std::function<void(int)> onHighlightCallback;
  std::function<void()> onCancelCallback;
};
