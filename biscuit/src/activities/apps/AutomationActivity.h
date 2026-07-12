#pragma once
#include <cstdint>
#include <vector>
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class AutomationActivity final : public Activity {
 public:
  explicit AutomationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Automation", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum State { RULE_LIST, EDIT_TRIGGER, EDIT_VALUE, EDIT_ACTION, CONFIRM_DELETE };
  State state = RULE_LIST;

  struct Rule {
    enum TriggerType : uint8_t { WIFI_APPEAR, WIFI_DISAPPEAR, TIMER, NONE };
    enum ActionType : uint8_t { LOG_EVENT, ENABLE_RF_SILENCE, SHOW_ALERT, NONE_ACT };
    TriggerType trigger = NONE;
    ActionType action = NONE_ACT;
    char triggerValue[33] = {};  // SSID or "HH:MM" time
    bool enabled = true;
  };

  std::vector<Rule> rules;
  static constexpr int MAX_RULES = 10;
  int ruleIndex = 0;
  int editFieldIndex = 0;
  ButtonNavigator buttonNavigator;

  // Editing state
  Rule editRule = {};

  static constexpr const char* RULES_PATH = "/biscuit/automation.dat";

  void loadRules();
  void saveRules();
  void renderRuleList() const;
  void renderEditTrigger() const;
  void renderEditAction() const;

  static const char* triggerName(Rule::TriggerType t);
  static const char* actionName(Rule::ActionType a);
};
