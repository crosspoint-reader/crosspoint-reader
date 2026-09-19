#pragma once

#include <memory>

#include "activities/UiListActivity.h"

class DictionaryFontSelectActivity final : public UiListActivity {
 public:
  DictionaryFontSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : UiListActivity("DictionaryFontSelect", renderer, mappedInput) {}

  void onEnter() override;

 private:
  struct FontName {
    char value[32];
  };

  int listCount() const override { return fontCount; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;

  std::unique_ptr<FontName[]> fontNames;
  std::unique_ptr<freeink::ui::ListItem[]> rowItems;
  int fontCount = 0;
};
