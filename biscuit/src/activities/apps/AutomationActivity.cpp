#include "AutomationActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>

#include <cstring>

#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

const char* AutomationActivity::triggerName(Rule::TriggerType t) {
  switch (t) {
    case Rule::WIFI_APPEAR:    return "WiFi Appear";
    case Rule::WIFI_DISAPPEAR: return "WiFi Disappear";
    case Rule::TIMER:          return "Timer";
    default:                   return "None";
  }
}

const char* AutomationActivity::actionName(Rule::ActionType a) {
  switch (a) {
    case Rule::LOG_EVENT:        return "Log Event";
    case Rule::ENABLE_RF_SILENCE: return "RF Silence";
    case Rule::SHOW_ALERT:       return "Show Alert";
    default:                     return "None";
  }
}

// ---------------------------------------------------------------------------
// Storage
// ---------------------------------------------------------------------------

void AutomationActivity::loadRules() {
  rules.clear();
  auto file = Storage.open(RULES_PATH);
  if (!file) return;
  uint8_t count = 0;
  file.read(&count, 1);
  for (int i = 0; i < count && i < MAX_RULES; i++) {
    Rule r = {};
    if (file.read(reinterpret_cast<uint8_t*>(&r), sizeof(Rule)) == sizeof(Rule)) {
      rules.push_back(r);
    }
  }
  file.close();
}

void AutomationActivity::saveRules() {
  Storage.mkdir("/biscuit");
  auto file = Storage.open(RULES_PATH, O_WRITE | O_CREAT | O_TRUNC);
  if (!file) return;
  uint8_t count = static_cast<uint8_t>(rules.size());
  file.write(&count, 1);
  for (const auto& r : rules) {
    file.write(reinterpret_cast<const uint8_t*>(&r), sizeof(Rule));
  }
  file.close();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void AutomationActivity::onEnter() {
  Activity::onEnter();
  loadRules();
  state = RULE_LIST;
  ruleIndex = 0;
  requestUpdate();
}

void AutomationActivity::onExit() { Activity::onExit(); }

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------

void AutomationActivity::loop() {
  // --- RULE_LIST ---
  if (state == RULE_LIST) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }

    // Item count: rules + optional "+ Add Rule" entry
    const int itemCount = static_cast<int>(rules.size()) + (rules.size() < MAX_RULES ? 1 : 0);

    buttonNavigator.onNext([this, itemCount] {
      ruleIndex = ButtonNavigator::nextIndex(ruleIndex, itemCount);
      requestUpdate();
    });
    buttonNavigator.onPrevious([this, itemCount] {
      ruleIndex = ButtonNavigator::previousIndex(ruleIndex, itemCount);
      requestUpdate();
    });

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      const bool isAddEntry = (ruleIndex == static_cast<int>(rules.size()));
      if (isAddEntry) {
        // New rule
        editRule = {};
        editFieldIndex = 0;
        state = EDIT_TRIGGER;
        requestUpdate();
      } else {
        // Toggle enabled/disabled on the selected rule
        rules[ruleIndex].enabled = !rules[ruleIndex].enabled;
        saveRules();
        requestUpdate();
      }
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      if (ruleIndex < static_cast<int>(rules.size())) {
        editRule = rules[ruleIndex];
        editFieldIndex = 0;
        state = EDIT_TRIGGER;
        requestUpdate();
      }
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      if (ruleIndex < static_cast<int>(rules.size())) {
        state = CONFIRM_DELETE;
        requestUpdate();
      }
      return;
    }

    return;
  }

  // --- CONFIRM_DELETE ---
  if (state == CONFIRM_DELETE) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (ruleIndex < static_cast<int>(rules.size())) {
        rules.erase(rules.begin() + ruleIndex);
        if (ruleIndex > 0 && ruleIndex >= static_cast<int>(rules.size())) {
          ruleIndex = static_cast<int>(rules.size()) - 1;
          if (ruleIndex < 0) ruleIndex = 0;
        }
        saveRules();
      }
      state = RULE_LIST;
      requestUpdate();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      state = RULE_LIST;
      requestUpdate();
      return;
    }
    return;
  }

  // --- EDIT_TRIGGER ---
  if (state == EDIT_TRIGGER) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      state = RULE_LIST;
      requestUpdate();
      return;
    }

    static constexpr int TRIGGER_COUNT = 3;  // WIFI_APPEAR, WIFI_DISAPPEAR, TIMER

    buttonNavigator.onNext([this] {
      editFieldIndex = ButtonNavigator::nextIndex(editFieldIndex, TRIGGER_COUNT);
      requestUpdate();
    });
    buttonNavigator.onPrevious([this] {
      editFieldIndex = ButtonNavigator::previousIndex(editFieldIndex, TRIGGER_COUNT);
      requestUpdate();
    });

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      editRule.trigger = static_cast<Rule::TriggerType>(editFieldIndex);
      editRule.triggerValue[0] = '\0';

      // Determine prompt based on trigger type
      const char* prompt = (editRule.trigger == Rule::TIMER) ? "Time (HH:MM)" : "SSID";
      const size_t maxLen = (editRule.trigger == Rule::TIMER) ? 5u : 32u;

      state = EDIT_VALUE;
      requestUpdate();

      startActivityForResult(
          std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, prompt, "", maxLen),
          [this](const ActivityResult& result) {
            if (result.isCancelled) {
              state = RULE_LIST;
            } else {
              const auto& text = std::get<KeyboardResult>(result.data).text;
              strncpy(editRule.triggerValue, text.c_str(), sizeof(editRule.triggerValue) - 1);
              editRule.triggerValue[sizeof(editRule.triggerValue) - 1] = '\0';
              editFieldIndex = 0;
              state = EDIT_ACTION;
            }
            requestUpdate();
          });
      return;
    }

    return;
  }

  // EDIT_VALUE is handled entirely by the KeyboardEntryActivity sub-activity.
  // No loop logic required here — the callback drives the state transition.

  // --- EDIT_ACTION ---
  if (state == EDIT_ACTION) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      state = RULE_LIST;
      requestUpdate();
      return;
    }

    static constexpr int ACTION_COUNT = 3;  // LOG_EVENT, ENABLE_RF_SILENCE, SHOW_ALERT

    buttonNavigator.onNext([this] {
      editFieldIndex = ButtonNavigator::nextIndex(editFieldIndex, ACTION_COUNT);
      requestUpdate();
    });
    buttonNavigator.onPrevious([this] {
      editFieldIndex = ButtonNavigator::previousIndex(editFieldIndex, ACTION_COUNT);
      requestUpdate();
    });

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      editRule.action = static_cast<Rule::ActionType>(editFieldIndex);
      editRule.enabled = true;

      // Replace existing rule or append new one
      if (ruleIndex < static_cast<int>(rules.size())) {
        rules[ruleIndex] = editRule;
      } else {
        rules.push_back(editRule);
        ruleIndex = static_cast<int>(rules.size()) - 1;
      }
      saveRules();
      editFieldIndex = 0;
      state = RULE_LIST;
      requestUpdate();
      return;
    }

    return;
  }
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void AutomationActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Automation");

  switch (state) {
    case RULE_LIST:      renderRuleList();    break;
    case EDIT_TRIGGER:   renderEditTrigger(); break;
    case EDIT_ACTION:    renderEditAction();  break;
    case CONFIRM_DELETE: GUI.drawPopup(renderer, "Delete this rule?"); break;
    case EDIT_VALUE:     /* handled by sub-activity */ break;
  }

  renderer.displayBuffer();
}

void AutomationActivity::renderRuleList() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  const int listTop = metrics.topPadding + metrics.headerHeight;
  const int listH = pageHeight - listTop - metrics.buttonHintsHeight;

  const int ruleCount = static_cast<int>(rules.size());
  const bool canAdd = (ruleCount < MAX_RULES);
  const int itemCount = ruleCount + (canAdd ? 1 : 0);

  GUI.drawList(
      renderer, Rect{0, listTop, pageWidth, listH}, itemCount, ruleIndex,
      [this, ruleCount, canAdd](int i) -> std::string {
        if (canAdd && i == ruleCount) return "+ Add Rule";
        const auto& r = rules[i];
        char buf[64];
        if (r.triggerValue[0] != '\0') {
          snprintf(buf, sizeof(buf), "%s: %s -> %s", triggerName(r.trigger), r.triggerValue,
                   actionName(r.action));
        } else {
          snprintf(buf, sizeof(buf), "%s -> %s", triggerName(r.trigger), actionName(r.action));
        }
        return buf;
      },
      nullptr, nullptr,
      [this, ruleCount, canAdd](int i) -> std::string {
        if (canAdd && i == ruleCount) return "";
        return rules[i].enabled ? "enabled" : "disabled";
      });

  const auto labels = mappedInput.mapLabels("Back", "Toggle", "Del", "Edit");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void AutomationActivity::renderEditTrigger() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  const int listTop = metrics.topPadding + metrics.headerHeight;
  const int listH = pageHeight - listTop - metrics.buttonHintsHeight;

  static constexpr int TRIGGER_COUNT = 3;
  GUI.drawList(
      renderer, Rect{0, listTop, pageWidth, listH}, TRIGGER_COUNT, editFieldIndex,
      [](int i) -> std::string {
        switch (i) {
          case 0: return "WiFi Appear";
          case 1: return "WiFi Disappear";
          case 2: return "Timer";
          default: return "";
        }
      });

  const auto labels = mappedInput.mapLabels("Cancel", "Select", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void AutomationActivity::renderEditAction() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  const int listTop = metrics.topPadding + metrics.headerHeight;
  const int listH = pageHeight - listTop - metrics.buttonHintsHeight;

  static constexpr int ACTION_COUNT = 3;
  GUI.drawList(
      renderer, Rect{0, listTop, pageWidth, listH}, ACTION_COUNT, editFieldIndex,
      [](int i) -> std::string {
        switch (i) {
          case 0: return "Log Event";
          case 1: return "RF Silence";
          case 2: return "Show Alert";
          default: return "";
        }
      });

  const auto labels = mappedInput.mapLabels("Cancel", "Select", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
