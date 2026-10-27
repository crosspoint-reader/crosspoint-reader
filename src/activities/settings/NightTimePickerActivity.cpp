#include "NightTimePickerActivity.h"

#include <GfxRenderer.h>

#include "MappedInputManager.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

NightTimePickerActivity::NightTimePickerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                 const StrId titleId, uint8_t CrossPointSettings::* field)
    : UiListActivity("NightTimePicker", renderer, mappedInput), titleId_(titleId), field_(field) {}

void NightTimePickerActivity::onEnter() {
  UiListActivity::onEnter();
  rowItems_.resize(nightschedule::SLOT_COUNT);
  for (uint8_t slot = 0; slot < nightschedule::SLOT_COUNT; slot++) {
    nightschedule::formatSlot(slot, SETTINGS.clockFormat == 1, labels_[slot], sizeof(labels_[slot]));
    rowItems_[slot].label = labels_[slot];
    rowItems_[slot].actionValue = static_cast<int16_t>(slot);
  }
  const uint8_t current = SETTINGS.*field_;
  nav.reset(current < nightschedule::SLOT_COUNT ? current : 0);
}

int NightTimePickerActivity::listCount() const { return nightschedule::SLOT_COUNT; }

const char* NightTimePickerActivity::headerTitle() const { return I18N.get(titleId_); }

void NightTimePickerActivity::activateIndex(const int index) {
  if (index < 0 || index >= listCount()) return;
  SETTINGS.*field_ = static_cast<uint8_t>(index);
  SETTINGS.saveToFile();
  finish();
}

void NightTimePickerActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 1;
  syncListViewport(screen, props);
  screen.list(props);
}
