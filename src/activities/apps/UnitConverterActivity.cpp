#include "UnitConverterActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cmath>
#include <cstdlib>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

const std::vector<UnitConverterActivity::Category>& UnitConverterActivity::getCategories() {
  static const std::vector<Category> cats = {
      {"Length",
       {{"Meter", "m", 1.0, 0},
        {"Kilometer", "km", 1000.0, 0},
        {"Centimeter", "cm", 0.01, 0},
        {"Millimeter", "mm", 0.001, 0},
        {"Mile", "mi", 1609.344, 0},
        {"Yard", "yd", 0.9144, 0},
        {"Foot", "ft", 0.3048, 0},
        {"Inch", "in", 0.0254, 0}}},
      {"Weight",
       {{"Kilogram", "kg", 1.0, 0},
        {"Gram", "g", 0.001, 0},
        {"Milligram", "mg", 0.000001, 0},
        {"Pound", "lb", 0.453592, 0},
        {"Ounce", "oz", 0.0283495, 0},
        {"Ton", "t", 1000.0, 0}}},
      {"Temperature",
       {{"Celsius", "C", 1.0, 0},
        {"Fahrenheit", "F", 0.5556, -17.7778},
        {"Kelvin", "K", 1.0, -273.15}}},
      {"Data",
       {{"Byte", "B", 1.0, 0},
        {"Kilobyte", "KB", 1024.0, 0},
        {"Megabyte", "MB", 1048576.0, 0},
        {"Gigabyte", "GB", 1073741824.0, 0},
        {"Terabyte", "TB", 1099511627776.0, 0},
        {"Bit", "bit", 0.125, 0}}},
      {"Speed",
       {{"m/s", "m/s", 1.0, 0},
        {"km/h", "km/h", 0.277778, 0},
        {"mph", "mph", 0.44704, 0},
        {"knot", "kn", 0.514444, 0}}}};
  return cats;
}

void UnitConverterActivity::onEnter() {
  Activity::onEnter();
  state = SELECT_CATEGORY;
  categoryIndex = 0;
  unitIndex = 0;
  valueStr = "1";
  cursorPos = 1;
  conversionResults.clear();
  requestUpdate();
}

void UnitConverterActivity::onExit() { Activity::onExit(); }

void UnitConverterActivity::computeConversions() {
  conversionResults.clear();
  auto& cats = getCategories();
  if (categoryIndex < 0 || categoryIndex >= static_cast<int>(cats.size())) return;
  auto& cat = cats[categoryIndex];
  if (unitIndex < 0 || unitIndex >= static_cast<int>(cat.units.size())) return;

  double inputVal = atof(valueStr.c_str());
  auto& fromUnit = cat.units[unitIndex];

  // Special handling for temperature
  bool isTemp = (std::string(cat.name) == "Temperature");

  for (int i = 0; i < static_cast<int>(cat.units.size()); i++) {
    if (i == unitIndex) continue;
    auto& toUnit = cat.units[i];
    double result;

    if (isTemp) {
      // Convert to Celsius first
      double celsius;
      if (fromUnit.abbrev[0] == 'C') {
        celsius = inputVal;
      } else if (fromUnit.abbrev[0] == 'F') {
        celsius = (inputVal - 32.0) * 5.0 / 9.0;
      } else {
        celsius = inputVal - 273.15;
      }
      // Convert from Celsius to target
      if (toUnit.abbrev[0] == 'C') {
        result = celsius;
      } else if (toUnit.abbrev[0] == 'F') {
        result = celsius * 9.0 / 5.0 + 32.0;
      } else {
        result = celsius + 273.15;
      }
    } else {
      double baseVal = inputVal * fromUnit.toBase;
      result = baseVal / toUnit.toBase;
    }

    char buf[64];
    if (fabs(result) >= 1000000 || (fabs(result) < 0.001 && result != 0)) {
      snprintf(buf, sizeof(buf), "%.4e %s", result, toUnit.abbrev);
    } else {
      snprintf(buf, sizeof(buf), "%.4f %s", result, toUnit.abbrev);
    }
    conversionResults.emplace_back(buf);
  }
}

void UnitConverterActivity::loop() {
  if (state == SELECT_CATEGORY) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }
    buttonNavigator.onNext([this] {
      categoryIndex = ButtonNavigator::nextIndex(categoryIndex, getCategories().size());
      requestUpdate();
    });
    buttonNavigator.onPrevious([this] {
      categoryIndex = ButtonNavigator::previousIndex(categoryIndex, getCategories().size());
      requestUpdate();
    });
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      unitIndex = 0;
      state = SELECT_UNIT;
      requestUpdate();
    }
    return;
  }

  if (state == SELECT_UNIT) {
    auto& cat = getCategories()[categoryIndex];
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      state = SELECT_CATEGORY;
      requestUpdate();
      return;
    }
    buttonNavigator.onNext([this, &cat] {
      unitIndex = ButtonNavigator::nextIndex(unitIndex, cat.units.size());
      requestUpdate();
    });
    buttonNavigator.onPrevious([this, &cat] {
      unitIndex = ButtonNavigator::previousIndex(unitIndex, cat.units.size());
      requestUpdate();
    });
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      valueStr = "1";
      cursorPos = 1;
      state = INPUT_VALUE;
      requestUpdate();
    }
    return;
  }

  if (state == INPUT_VALUE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      state = SELECT_UNIT;
      requestUpdate();
      return;
    }

    // Up/Down = change digit at cursor
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      if (cursorPos > 0 && cursorPos <= static_cast<int>(valueStr.size())) {
        char& c = valueStr[cursorPos - 1];
        if (c >= '0' && c < '9')
          c++;
        else if (c == '9')
          c = '0';
      }
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      if (cursorPos > 0 && cursorPos <= static_cast<int>(valueStr.size())) {
        char& c = valueStr[cursorPos - 1];
        if (c > '0' && c <= '9')
          c--;
        else if (c == '0')
          c = '9';
      }
      requestUpdate();
    }

    // Left/Right = move cursor
    if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      if (cursorPos > 1) cursorPos--;
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      if (cursorPos < static_cast<int>(valueStr.size())) {
        cursorPos++;
      } else {
        // Add a new digit
        if (valueStr.size() < 15) {
          valueStr += '0';
          cursorPos = valueStr.size();
        }
      }
      requestUpdate();
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      computeConversions();
      state = RESULTS;
      requestUpdate();
    }
    return;
  }

  if (state == RESULTS) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      state = INPUT_VALUE;
      requestUpdate();
    }
  }
}

void UnitConverterActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_UNIT_CONVERTER));

  switch (state) {
    case SELECT_CATEGORY:
      renderCategorySelect();
      break;
    case SELECT_UNIT:
      renderUnitSelect();
      break;
    case INPUT_VALUE:
      renderInputValue();
      break;
    case RESULTS:
      renderResults();
      break;
  }

  renderer.displayBuffer();
}

void UnitConverterActivity::renderCategorySelect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  auto& cats = getCategories();
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(cats.size()), categoryIndex,
      [&cats](int i) { return std::string(cats[i].name); });

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void UnitConverterActivity::renderUnitSelect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  auto& cat = getCategories()[categoryIndex];
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(cat.units.size()), unitIndex,
      [&cat](int i) { return std::string(cat.units[i].name); }, nullptr, nullptr,
      [&cat](int i) { return std::string(cat.units[i].abbrev); });

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void UnitConverterActivity::renderInputValue() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageHeight = renderer.getScreenHeight();
  int y = pageHeight / 2 - 40;

  auto& cat = getCategories()[categoryIndex];
  auto& unit = cat.units[unitIndex];
  char label[64];
  snprintf(label, sizeof(label), "Enter value in %s:", unit.name);
  renderer.drawCenteredText(UI_10_FONT_ID, y, label);
  y += 40;

  // Show value with cursor indicator
  std::string display = valueStr;
  if (cursorPos >= 1 && cursorPos <= static_cast<int>(display.size())) {
    display.insert(cursorPos, "]");
    display.insert(cursorPos - 1, "[");
  }
  renderer.drawCenteredText(UI_12_FONT_ID, y, display.c_str(), true, EpdFontFamily::BOLD);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "Convert", "<", ">");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  GUI.drawSideButtonHints(renderer, "+", "-");
}

void UnitConverterActivity::renderResults() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);

  auto& cat = getCategories()[categoryIndex];
  auto& unit = cat.units[unitIndex];
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2;

  char fromBuf[64];
  snprintf(fromBuf, sizeof(fromBuf), "%s %s =", valueStr.c_str(), unit.abbrev);
  renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, y, fromBuf, true, EpdFontFamily::BOLD);
  y += lineH + 15;

  for (auto& line : conversionResults) {
    if (y > pageHeight - metrics.buttonHintsHeight - 10) break;
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding + 10, y, line.c_str());
    y += lineH + 3;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
