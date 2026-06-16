#pragma once
#include <functional>
#include <string>
#include <vector>

#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"

class OptionPopup {
 public:
  void show(const char* title, const std::vector<std::string>& options, int currentIndex,
            std::function<void(int)> onSelect) {
    this->title = title;
    this->options = options;
    this->selectedIndex = currentIndex;
    this->onSelectCallback = std::move(onSelect);
    this->active = true;
  }

  bool handleInput(MappedInputManager& input, const std::function<void()>& requestUpdate) {
    if (!active) return false;

    if (input.wasPressed(MappedInputManager::Button::Up) || input.wasPressed(MappedInputManager::Button::Left)) {
      selectedIndex = (selectedIndex - 1 + options.size()) % options.size();
      requestUpdate();
      return true;
    } else if (input.wasPressed(MappedInputManager::Button::Down) ||
               input.wasPressed(MappedInputManager::Button::Right)) {
      selectedIndex = (selectedIndex + 1) % options.size();
      requestUpdate();
      return true;
    } else if (input.wasPressed(MappedInputManager::Button::Confirm)) {
      active = false;
      if (onSelectCallback) onSelectCallback(selectedIndex);
      requestUpdate();
      return true;
    } else if (input.wasPressed(MappedInputManager::Button::Back)) {
      active = false;
      requestUpdate();
      return true;
    }
    return true;  // Consume all input while active
  }

  void render(GfxRenderer& renderer) const {
    if (!active) return;
    GUI.drawOptionPopup(renderer, title.c_str(), options, selectedIndex);
  }

  bool isActive() const { return active; }

 private:
  bool active = false;
  std::string title;
  std::vector<std::string> options;
  int selectedIndex = 0;
  std::function<void(int)> onSelectCallback;
};
