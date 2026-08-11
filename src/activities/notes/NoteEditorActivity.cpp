#include "NoteEditorActivity.h"

#include <BleKeyboardHost.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr const char* TAG = "NOTE";
constexpr unsigned long AUTOSAVE_MS = 2000;
constexpr size_t MAX_NOTE_BYTES = 64 * 1024;
}

void NoteEditorActivity::onEnter() {
  Activity::onEnter();
  load();
  cursor_ = text_.size();
  ensureBleConnected();
  lastAutosaveMs_ = millis();
  status_ = createdNow_ ? tr(STR_NOTE_CREATED) : tr(STR_NOTE_OPENED);
  requestUpdate();
}

void NoteEditorActivity::onExit() {
  Activity::onExit();
  if (dirty_) save();
  if (bleStarted_) {
    BleHid.end();
    bleStarted_ = false;
  }
}

void NoteEditorActivity::ensureBleConnected() {
  if (BleHid.begin("CrossPoint X4")) {
    bleStarted_ = true;
    if (!BleHid.isConnected() && BleHid.pairedCount() > 0) {
      BleHid.connect(BleHid.paired(0).addr);
      status_ = tr(STR_NOTE_BLUETOOTH_CONNECTING);
    }
  } else {
    status_ = tr(STR_BLUETOOTH_UNAVAILABLE);
  }
}

bool NoteEditorActivity::load() {
  text_.clear();
  if (!Storage.exists(path_.c_str())) {
    dirty_ = true;
    return true;
  }
  HalFile f;
  if (!Storage.openFileForRead(TAG, path_, f)) return false;
  while (f.available() && text_.size() < MAX_NOTE_BYTES) {
    text_.push_back(static_cast<char>(f.read()));
  }
  f.close();
  dirty_ = false;
  savedOnce_ = true;
  return true;
}

bool NoteEditorActivity::save() {
  const auto slash = path_.find_last_of('/');
  if (slash != std::string::npos) {
    const std::string dir = path_.substr(0, slash);
    if (!dir.empty() && !Storage.exists(dir.c_str())) Storage.mkdir(dir.c_str());
  }
  HalFile f;
  if (!Storage.openFileForWrite(TAG, path_, f)) {
    status_ = tr(STR_NOTE_SAVE_FAILED);
    requestUpdate();
    return false;
  }
  if (!text_.empty()) f.write(reinterpret_cast<const uint8_t*>(text_.data()), text_.size());
  f.flush();
  f.close();
  dirty_ = false;
  savedOnce_ = true;
  status_ = tr(STR_NOTE_SAVED);
  return true;
}

void NoteEditorActivity::insertChar(char ch) {
  if (text_.size() >= MAX_NOTE_BYTES) return;
  text_.insert(text_.begin() + std::min(cursor_, text_.size()), ch);
  cursor_++;
  dirty_ = true;
}

void NoteEditorActivity::insertText(const char* s) {
  if (!s) return;
  while (*s) insertChar(*s++);
}

void NoteEditorActivity::backspace() {
  if (cursor_ == 0 || text_.empty()) return;
  text_.erase(text_.begin() + cursor_ - 1);
  cursor_--;
  dirty_ = true;
}

void NoteEditorActivity::moveCursorLeft() {
  if (cursor_ > 0) cursor_--;
}

void NoteEditorActivity::moveCursorRight() {
  if (cursor_ < text_.size()) cursor_++;
}

void NoteEditorActivity::handleBleKeys() {
  if (!bleStarted_) return;
  BleHid.poll();
  if (BleHid.isConnected()) status_ = tr(STR_NOTE_BLUETOOTH_CONNECTED);

  freeink::KeyEvent ev;
  bool changed = false;
  while (BleHid.popKey(ev)) {
    if (!ev.pressed) continue;
    if (ev.ch) {
      insertChar(ev.ch);
      changed = true;
      continue;
    }
    switch (ev.special) {
      case freeink::SpecialKey::Enter:
        insertChar('\n');
        changed = true;
        break;
      case freeink::SpecialKey::Tab:
        insertText("  ");
        changed = true;
        break;
      case freeink::SpecialKey::Backspace:
      case freeink::SpecialKey::Delete:
        backspace();
        changed = true;
        break;
      case freeink::SpecialKey::Left:
        moveCursorLeft();
        changed = true;
        break;
      case freeink::SpecialKey::Right:
        moveCursorRight();
        changed = true;
        break;
      case freeink::SpecialKey::Escape:
        closeRequested_ = true;
        break;
      default:
        break;
    }
  }
  if (changed) requestUpdate();
}

void NoteEditorActivity::loop() {
  handleBleKeys();

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) confirmHeld_ = true;
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (confirmHeld_) {
      save();
      requestUpdate();
    }
    confirmHeld_ = false;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back) || closeRequested_) {
    if (dirty_) save();
    finish();
    return;
  }

  if (dirty_ && millis() - lastAutosaveMs_ >= AUTOSAVE_MS) {
    save();
    lastAutosaveMs_ = millis();
    requestUpdate();
  }
}

std::vector<std::string> NoteEditorActivity::visibleLines(int maxLines) const {
  std::vector<std::string> lines;
  lines.reserve(maxLines);
  size_t start = 0;
  size_t lineStart = 0;
  size_t currentLine = 0;
  for (size_t i = 0; i <= text_.size(); ++i) {
    if (i == text_.size() || text_[i] == '\n') {
      if (cursor_ >= lineStart && cursor_ <= i) {
        start = currentLine > static_cast<size_t>(maxLines / 2) ? currentLine - maxLines / 2 : 0;
      }
      lineStart = i + 1;
      currentLine++;
    }
  }

  currentLine = 0;
  lineStart = 0;
  for (size_t i = 0; i <= text_.size(); ++i) {
    if (i == text_.size() || text_[i] == '\n') {
      if (currentLine >= start && lines.size() < static_cast<size_t>(maxLines)) {
        std::string line = text_.substr(lineStart, i - lineStart);
        if (cursor_ >= lineStart && cursor_ <= i) {
          line.insert(cursor_ - lineStart, "|");
        }
        lines.push_back(std::move(line));
      }
      lineStart = i + 1;
      currentLine++;
    }
  }
  if (lines.empty()) lines.push_back("|");
  return lines;
}

void NoteEditorActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  const auto title = renderer.truncatedText(UI_12_FONT_ID, path_.c_str(), width - 20, EpdFontFamily::BOLD);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, title.c_str(),
                 dirty_ ? tr(STR_NOTE_UNSAVED) : tr(STR_NOTE_SAVED));

  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID) + 2;
  const int maxLines = std::max(1, (height - top - metrics.buttonHintsHeight - metrics.verticalSpacing) / lineHeight);
  auto lines = visibleLines(maxLines);
  int y = top;
  for (const auto& line : lines) {
    auto clipped = renderer.truncatedText(UI_10_FONT_ID, line.c_str(), width - 16);
    renderer.drawText(UI_10_FONT_ID, 8, y, clipped.c_str());
    y += lineHeight;
  }

  if (!status_.empty()) {
    auto st = renderer.truncatedText(SMALL_FONT_ID, status_.c_str(), width - 16);
    renderer.drawText(SMALL_FONT_ID, 8, height - metrics.buttonHintsHeight - 14, st.c_str());
  }
  const auto labels = mappedInput.mapLabels(tr(STR_CLOSE), tr(STR_SAVE), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
