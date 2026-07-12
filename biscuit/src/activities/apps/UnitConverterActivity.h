#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class UnitConverterActivity final : public Activity {
 public:
  explicit UnitConverterActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("UnitConverter", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum State { SELECT_CATEGORY, SELECT_UNIT, INPUT_VALUE, RESULTS };

  State state = SELECT_CATEGORY;
  int categoryIndex = 0;
  int unitIndex = 0;
  ButtonNavigator buttonNavigator;

  // Input value
  std::string valueStr;
  int cursorPos = 0;

  // Categories and units
  struct UnitDef {
    const char* name;
    const char* abbrev;
    double toBase;   // multiply by this to convert to base unit
    double offset;   // for temperature: offset after multiplication
  };

  struct Category {
    const char* name;
    std::vector<UnitDef> units;
  };

  static const std::vector<Category>& getCategories();
  std::vector<std::string> conversionResults;

  void computeConversions();

  void renderCategorySelect() const;
  void renderUnitSelect() const;
  void renderInputValue() const;
  void renderResults() const;
};
