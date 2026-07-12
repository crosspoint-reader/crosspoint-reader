#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"
#include "util/ButtonNavigator.h"

/**
 * Generic category submenu for apps.
 * Takes a list of app names and factory functions to create them.
 */
class AppCategoryActivity final : public Activity {
 public:
  struct AppEntry {
    const char* nameStrId;  // will be resolved via tr() at render time
    const char* description;  // one-line description shown as subtitle
    UIIcon icon;
    std::function<std::unique_ptr<Activity>(GfxRenderer&, MappedInputManager&)> factory;
    bool isSectionHeader = false;
    std::function<bool()> hasActiveState = nullptr;  // returns true if app has saved state
  };

  static AppEntry SectionHeader(const char* label) {
    return AppEntry{label, nullptr, UIIcon::File, nullptr, true};
  }

  explicit AppCategoryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const char* title,
                                std::vector<AppEntry> entries, bool requiresDisclaimer = false, int categoryIndex = -1)
      : Activity("AppCategory", renderer, mappedInput),
        title(title),
        entries(std::move(entries)),
        requiresDisclaimer(requiresDisclaimer),
        categoryIndex(categoryIndex) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  const char* title;
  std::vector<AppEntry> entries;
  bool requiresDisclaimer;
  bool disclaimerShown = false;
  int categoryIndex = -1;

  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  bool backPressedHere = false;  // Guard against stale Back release from child activity
};
