#include "BleSettingsActivity.h"

#include <BlePageTurner.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "I18nKeys.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
constexpr int16_t ROW_ENABLE = -1;
constexpr int16_t ROW_SCAN = -2;
constexpr int16_t ROW_NONE = -3;
constexpr int16_t ROW_LEARN = -4;
constexpr int16_t ROW_FOUND = 0;   // + index of a device the scan found
constexpr int16_t ROW_BOND = 100;  // + index of a paired remote
constexpr uint32_t SCAN_MS = 15000;
}  // namespace

void BleSettingsActivity::onEnter() {
  UiListActivity::onEnter();
  // A saved opt-in brings the radio up here, so scanning and pairing work at once.
  if (SETTINGS.ble.enabled) bleturner::switchOn();
  refresh();
  requestUpdate();
}

void BleSettingsActivity::loop() {
  UiListActivity::loop();
  if (millis() - lastPollMs < 250) return;
  lastPollMs = millis();
  // A remote being paired that links becomes the chosen one: save it.
  if (SETTINGS.ble.enabled && bleturner::service()) SETTINGS.saveToFile();
  if (learnStage != 0) stepLearning();
  if (signature() == shownSignature) return;  // an e-ink repaint only when something changed
  refresh();
  requestUpdate();
}

const char* BleSettingsActivity::headerTitle() const { return tr(STR_BT_PAGE_TURNER); }

uint32_t BleSettingsActivity::signature() const {
  const auto st = bleturner::status();
  return (st.running ? 1u : 0u) | (st.scanning ? 2u : 0u) | (st.connected ? 4u : 0u) | (st.connecting ? 8u : 0u) |
         (st.stopping ? 16u : 0u) | (SETTINGS.ble.enabled ? 32u : 0u) |
         static_cast<uint32_t>(bleturner::bondCount()) << 8 | static_cast<uint32_t>(bleturner::foundCount()) << 16;
}

void BleSettingsActivity::refresh() {
  RenderLock lock(*this);
  shownSignature = signature();
  const auto st = bleturner::status();
  char failure[48];
  if (!SETTINGS.ble.enabled || st.stopping) {
    status = tr(STR_STATE_OFF);
  } else if (bleturner::takeConnectFailure(failure, sizeof(failure))) {
    status = failure;
  } else if (!st.running) {
    status = tr(STR_BT_START_FAILED);
  } else if (st.connected) {
    status = bleturner::linked().name;
  } else if (st.connecting) {
    status = tr(STR_CONNECTING);
  } else if (st.scanning) {
    status = tr(STR_SCANNING);
  } else {
    status = tr(STR_STATE_ON);
  }

  // Labels first: the rows point into them.
  const uint8_t found = st.running ? bleturner::foundCount() : 0;
  const uint8_t bonds = st.running ? bleturner::bondCount() : 0;
  labels.clear();
  for (uint8_t i = 0; i < found; ++i) labels.emplace_back(bleturner::found(i).name);
  for (uint8_t i = 0; i < bonds; ++i) {
    const auto peer = bleturner::bond(i);
    labels.emplace_back(peer.name[0] != '\0' ? peer.name : peer.addr);
  }

  rows.clear();
  fui::ListItem enable;
  enable.label = tr(STR_BT_PAGE_TURNER);
  enable.subtitle = status.c_str();
  enable.toggle = true;
  enable.toggleChecked = SETTINGS.ble.enabled != 0;
  enable.actionValue = ROW_ENABLE;
  rows.push_back(enable);
  if (!st.running) return;

  fui::ListItem scan;
  scan.label = st.scanning ? tr(STR_BT_STOP_SCAN) : tr(STR_BT_SCAN);
  scan.actionValue = ROW_SCAN;
  rows.push_back(scan);
  if (st.connected) {
    fui::ListItem learn;
    learn.label = tr(STR_BT_LEARN_KEYS);
    learn.subtitle = learnNote;
    learn.actionValue = ROW_LEARN;
    rows.push_back(learn);
  }
  for (uint8_t i = 0; i < found; ++i) {
    fui::ListItem item;
    item.label = labels[i].c_str();
    item.actionValue = static_cast<int16_t>(ROW_FOUND + i);
    rows.push_back(item);
  }
  for (uint8_t i = 0; i < bonds; ++i) {
    fui::ListItem item;
    item.label = labels[found + i].c_str();
    item.value = tr(STR_FORGET_BUTTON);
    item.actionValue = static_cast<int16_t>(ROW_BOND + i);
    if (i == 0) item.sectionHeading = tr(STR_BT_PAIRED_REMOTES);
    rows.push_back(item);
  }
  if (bonds == 0) {
    fui::ListItem none;
    none.label = tr(STR_BT_NO_REMOTES);
    none.enabled = false;
    none.actionValue = ROW_NONE;
    none.sectionHeading = tr(STR_BT_PAIRED_REMOTES);
    rows.push_back(none);
  }
  if (nav.selected >= static_cast<int>(rows.size())) nav.selected = static_cast<int>(rows.size()) - 1;
}

void BleSettingsActivity::activateIndex(const int index) {
  if (index < 0 || index >= static_cast<int>(rows.size()) || !rows[index].enabled) return;
  nav.selected = index;
  app.clearTapFlash();
  const int16_t code = rows[index].actionValue;
  if (code == ROW_ENABLE) {
    SETTINGS.ble.enabled = SETTINGS.ble.enabled ? 0 : 1;
    if (SETTINGS.ble.enabled) {
      bleturner::switchOn();
    } else {
      bleturner::switchOff();
    }
    SETTINGS.saveToFile();
  } else if (code == ROW_SCAN) {
    bleturner::scan(bleturner::status().scanning ? 0 : SCAN_MS);
  } else if (code == ROW_LEARN) {
    startLearning();
  } else if (code >= ROW_BOND) {
    const std::string addr = bleturner::bond(static_cast<uint8_t>(code - ROW_BOND)).addr;
    if (bleturner::forget(addr.c_str())) SETTINGS.saveToFile();
  } else if (code >= ROW_FOUND) {
    // The remote paired here becomes the one the reader reconnects to once it links (service());
    // a pairing that fails keeps the remote chosen before.
    const auto peer = bleturner::found(static_cast<uint8_t>(code - ROW_FOUND));
    const std::string addr = peer.addr;
    const std::string name = peer.name;
    bleturner::scan(0);
    bleturner::pair(addr.c_str(), name.c_str());
  } else {
    return;
  }
  refresh();
  requestUpdate();
}

void BleSettingsActivity::startLearning() {
  bleturner::Event ev;
  while (bleturner::pollEvent(ev)) {
  }  // presses from before the row was chosen are not the answer
  learnStage = 1;
  learnCode = 0;
  learnSinceMs = millis();
  learnNote = tr(STR_BT_PRESS_NEXT);
}

void BleSettingsActivity::stepLearning() {
  const uint8_t stage = learnStage;
  bleturner::Event ev;
  while (learnStage == stage && bleturner::pollEvent(ev)) {
    if (ev.pressed) {
      if (learnStage == 2 && ev.code == nextCode) continue;  // one button cannot turn both ways
      learnCode = ev.code;
      learnSinceMs = millis();
    } else if (ev.code == learnCode) {
      if (ev.wasRest) {
        learnCode = 0;  // the remote idling on a non-zero byte, not a button
      } else {
        keepLearned();
      }
    }
  }
  if (learnStage == stage) {
    // A remote that never reports the release: the press alone is the answer.
    if (learnCode != 0 && millis() - learnSinceMs >= bleturner::kReleaseWaitMs) {
      keepLearned();
    } else if (learnCode == 0 && millis() - learnSinceMs >= bleturner::kWaitMs) {
      learnStage = 0;
      learnNote = tr(STR_BT_NO_KEY);
    }
  }
  if (learnStage != stage) {
    refresh();
    requestUpdate();
  }
}

void BleSettingsActivity::keepLearned() {
  // The remote's saved table, or a new one from its built-in default (a Free3 keeps its
  // chapter button), so both page buttons go by their raw edges like the chapter button.
  const auto peer = bleturner::linked();
  bleturner::RemoteTable* table =
      bleturner::editableTable(SETTINGS.ble.remotes, SETTINGS.ble.remoteCount, peer.addr, peer.name);
  const auto action = learnStage == 1 ? bleturner::Action::NextPage : bleturner::Action::PrevPage;
  if (table == nullptr || !bleturner::learn(*table, action, learnCode, false)) {
    learnStage = 0;
    learnNote = tr(STR_BT_KEYS_NOT_SAVED);
    return;
  }
  SETTINGS.saveToFile();
  if (learnStage == 1) nextCode = learnCode;
  learnCode = 0;
  learnSinceMs = millis();
  learnStage = learnStage == 1 ? 2 : 0;
  learnNote = learnStage == 2 ? tr(STR_BT_PRESS_PREV) : tr(STR_BT_KEYS_SAVED);
}

void BleSettingsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                                      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                                      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)),
                                      static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  fui::ListProps props;
  props.items = rows.data();
  props.count = static_cast<uint16_t>(rows.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  syncListViewport(screen, props);
  screen.list(props);
}
