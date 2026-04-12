#pragma once
#include <vector>

#include "Activity.h"
#include "settings/SettingInfo.h"
#include "util/ButtonNavigator.h"

struct Rect;

// Base class for activities that display a scrollable list of SettingInfo items.
// Provides common navigation, toggle/cycle logic, and drawList rendering.
//
// Subclasses populate `menuItems` (typically in the constructor or onEnter()),
// then rely on the default loop()/onEnter() or override selectively.
//
// === Minimal example ===
//
//   class MyMenuActivity final : public MenuListActivity {
//    public:
//     explicit MyMenuActivity(GfxRenderer& r, MappedInputManager& m)
//         : MenuListActivity("MyMenu", r, m) {
//       // Separator header
//       menuItems.push_back(SettingInfo::Separator(StrId::STR_MY_SECTION));
//
//       // Toggle bound to a CrossPointSettings field
//       menuItems.push_back(SettingInfo::Toggle(StrId::STR_MY_TOGGLE, &CrossPointSettings::myFlag));
//
//       // Enum bound to a CrossPointSettings field
//       menuItems.push_back(SettingInfo::Enum(StrId::STR_MY_ENUM, &CrossPointSettings::myEnum,
//           {StrId::STR_OPTION_A, StrId::STR_OPTION_B, StrId::STR_OPTION_C}));
//
//       // DynamicEnum with local state (getter/setter lambdas)
//       menuItems.push_back(SettingInfo::DynamicEnum(StrId::STR_MY_DYNAMIC,
//           {StrId::STR_LOW, StrId::STR_HIGH},
//           [this]() -> uint8_t { return localValue; },
//           [this](uint8_t v)   { localValue = v; }));
//
//       // Action item (handled in onActionSelected)
//       menuItems.push_back(SettingInfo::Action(StrId::STR_DO_SOMETHING, SettingAction::None));
//     }
//
//     // render(): draw header/footer around the list
//     void render(RenderLock&&) override {
//       renderer.clearScreen();
//       const Rect r = UITheme::getContentRect(renderer, true, false);
//       drawMenuList(r);                            // <-- draws the item list
//       GUI.drawButtonHints(renderer, ...);
//       renderer.displayBuffer();
//     }
//
//    private:
//     uint8_t localValue = 0;
//
//     // Called when an ACTION item is confirmed
//     void onActionSelected(int index) override {
//       if (menuItems[index].nameId == StrId::STR_DO_SOMETHING) { /* ... */ }
//     }
//
//     // Called after a TOGGLE/ENUM/VALUE is cycled
//     void onSettingToggled(int) override { SETTINGS.saveToFile(); }
//   };
//
// The base class provides:
//   onEnter()  — wires up the selectable-predicate (skips separators) and requestUpdate().
//   loop()     — handles Back (→ onBackPressed), Confirm (→ toggleCurrentItem), and nav.
//   drawMenuList(rect) — calls GUI.drawList with SettingInfo-based title/value lambdas.
//   getItemValueString(i) — override for custom per-item value display.
//   onBackPressed()    — override to customise Back behaviour (default: finish()).
//
class MenuListActivity : public Activity {
 protected:
  std::vector<SettingInfo> menuItems;
  int selectedIndex = 0;
  ButtonNavigator buttonNavigator;

  // Call after building/rebuilding menuItems to wire up the selectable predicate.
  void initMenuList();

  // Handle up/down navigation via buttonNavigator.  Call from loop() if overriding.
  void handleNavigation();

  // Toggle/cycle the currently selected item.  For ACTION items, delegates to onActionSelected().
  void toggleCurrentItem();

  // Draw the list into the given rect using GUI.drawList().
  void drawMenuList(const Rect& rect);

  // Override to provide custom value display for specific items.
  // Default returns SettingInfo::getDisplayValue().
  [[nodiscard]] virtual std::string getItemValueString(int index) const;

  // Called when an ACTION-type item is confirmed.  Override to handle actions.
  virtual void onActionSelected(int index) {}

  // Called when Back is pressed.  Default calls finish().
  virtual void onBackPressed() { finish(); }

  // Called after a TOGGLE/ENUM/VALUE item has been toggled.
  // Override to persist changes or trigger side-effects.
  virtual void onSettingToggled(int /*index*/) {}

 public:
  using Activity::Activity;

  void onEnter() override;
  void loop() override;
};
