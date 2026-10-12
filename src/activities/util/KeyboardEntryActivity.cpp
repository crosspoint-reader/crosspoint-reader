#include "KeyboardEntryActivity.h"

#include <I18n.h>

#include <algorithm>
#include <cstring>

#include "KeyboardLayoutSet.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/themes/ButtonGeometry.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {

constexpr fui::ActionId ACTION_KEY = 1;
constexpr int KEYBOARD_MIN_GAP = 6;
constexpr int KEYBOARD_PANEL_PADDING = 4;
constexpr int BUTTON_KEY_HEIGHT = 42;
constexpr int BUTTON_KEYBOARD_HINT_GAP = 4;

int keyboardGap(const ThemeMetrics& metrics) { return std::max(metrics.keyboardKeySpacing, KEYBOARD_MIN_GAP); }

int keyboardKeysHeight(const ThemeMetrics& metrics, const int rows, const bool hasTouch) {
  const int keyHeight = hasTouch ? metrics.keyboardKeyHeight : BUTTON_KEY_HEIGHT;
  return rows * keyHeight + (rows > 1 ? (rows - 1) * keyboardGap(metrics) : 0) + KEYBOARD_PANEL_PADDING * 2;
}

// The screen area outside the button hints. The hints move with the orientation
// (the top edge upside down, a side in landscape), so every edge below is taken
// from here rather than from the screen.
Rect keyboardContentRect(const GfxRenderer& renderer) {
  return UITheme::getContentRect(renderer, true, ButtonGeometry::sideKeysOnOppositeEdges());
}

// ---------------------------------------------------------------------------
// URL layers. A URL layer is the English letter layer with its bottom row
// swapped for this one: URLs have no spaces, so the space slot goes to ":",
// "/", "." and the snippet-panel toggle (the old keyboard did the same with its
// "URL" key). The snippet panel is a layer of its own.
// ---------------------------------------------------------------------------

constexpr uint8_t URL_KEY_WIDTH = 2;
constexpr uint8_t URL_WIDE_CONTROL_WIDTH = 3;

#define UK(label, output, value) \
  fui::KeyboardKey { label, output, fui::KeyKind::Normal, fui::StateNormal, value, URL_KEY_WIDTH, true, nullptr }
#define UKW(label, output, value, units) \
  fui::KeyboardKey { label, output, fui::KeyKind::Normal, fui::StateNormal, value, units *URL_KEY_WIDTH, true, nullptr }
#define UKS(label, kind, value, units) \
  fui::KeyboardKey { label, nullptr, kind, fui::StateNormal, value, units *URL_KEY_WIDTH, true, nullptr }
#define UK15(label, kind, value) \
  fui::KeyboardKey { label, nullptr, kind, fui::StateNormal, value, URL_WIDE_CONTROL_WIDTH, true, nullptr }

constexpr int16_t URL_PANEL_VALUE = -3;  // mirrors KeyboardEntryActivity::URL_PANEL_KEY

const fui::KeyboardKey URL_BOTTOM[] = {UKS("?123", fui::KeyKind::Mode, fui::QWERTY_KEY_MODE, 2),
                                       UK(":", ":", ':'),
                                       UK("/", "/", '/'),
                                       UK(".", ".", '.'),
                                       UKW("URL", nullptr, URL_PANEL_VALUE, 3),
                                       UKS("OK", fui::KeyKind::Ok, fui::QWERTY_KEY_ENTER, 2)};

// Snippet keys: multi-character outputs, with stable ids above the localized-key
// range so they never collide with layout key ids.
const fui::KeyboardKey URL_SNIP_ROW1[] = {UK("https://", "https://", 2001), UK("www.", "www.", 2002),
                                          UK(".com", ".com", 2003)};
const fui::KeyboardKey URL_SNIP_ROW2[] = {UK("http://", "http://", 2004), UK("192.168.", "192.168.", 2005),
                                          UK(".org", ".org", 2006)};
const fui::KeyboardKey URL_SNIP_ROW3[] = {UK("/opds", "/opds", 2007), UK(":8080", ":8080", 2008),
                                          UK(".net", ".net", 2009)};
const fui::KeyboardKey URL_SNIP_BOTTOM[] = {UKS("abc", fui::KeyKind::Mode, fui::QWERTY_KEY_MODE, 2),
                                            UKW("URL", nullptr, URL_PANEL_VALUE, 3),
                                            UK15("Del", fui::KeyKind::Delete, fui::QWERTY_KEY_BACKSPACE),
                                            UKS("OK", fui::KeyKind::Ok, fui::QWERTY_KEY_ENTER, 2)};

#undef UK
#undef UKW
#undef UKS
#undef UK15

const fui::KeyboardRow URL_SNIP_ROWS[] = {
    {URL_SNIP_ROW1, 3, 0}, {URL_SNIP_ROW2, 3, 0}, {URL_SNIP_ROW3, 3, 0}, {URL_SNIP_BOTTOM, 4, 0}};
const fui::KeyboardLayout URL_SNIPPET_LAYOUT{URL_SNIP_ROWS, 4};

}  // namespace

void KeyboardEntryActivity::onEnter() {
  Activity::onEnter();
  cursorPos = text.length();
  // URL layers are English; everything else opens on the UI language's layout.
  layoutId = inputType == InputType::Url ? fui::KeyboardLayoutId::QwertyEn : keyboard_layouts::startingLayout();
  // The key only earns its slot in the bottom row with somewhere to go.
  const uint16_t enabledLayouts = keyboard_layouts::enabled();
  showLangKey = (enabledLayouts & (enabledLayouts - 1)) != 0;
  shifted = false;
  symbols = false;
  urlPanel = false;
  inputLayerKey = UINT16_MAX;
  cursorMode = false;
  togglePos = false;
  passwordVisible = false;
  selRow = 0;
  selCol = 0;
  hintVisible = false;
  rightHeld = false;
  rightLongHandled = false;
  savedCursorPos = 0;
  rightStartCursorPos = 0;
  touchRouter.reset();
  touchRouter.holdMs = TOUCH_LONG_PRESS_MS;
  touchRouter.overrideHoldMs = TOUCH_DEL_LONG_PRESS_MS;
  interactionsReady = false;
  requestUpdate();
}

void KeyboardEntryActivity::onExit() { Activity::onExit(); }

const fui::KeyboardLayout& KeyboardEntryActivity::buildLayer(fui::KeyboardLayoutBuffer& buffer) const {
  if (symbols) return fui::buildKeyboardLayout(buffer, layoutId, shifted, true);
  if (inputType != InputType::Url) {
    return fui::buildKeyboardLayout(buffer, layoutId, shifted, false, /*numberRow=*/true, showLangKey);
  }
  if (urlPanel) return URL_SNIPPET_LAYOUT;
  // The English layer with a digit row, its bottom row swapped for the URL one.
  // The rows live in `buffer`, so the swap edits this build and nothing shared.
  const fui::KeyboardLayout& layer =
      fui::buildKeyboardLayout(buffer, fui::KeyboardLayoutId::QwertyEn, shifted, false, /*numberRow=*/true);
  buffer.rows[layer.rowCount - 1] = fui::KeyboardRow{URL_BOTTOM, sizeof(URL_BOTTOM) / sizeof(URL_BOTTOM[0]), 0};
  return layer;
}

const fui::KeyboardLayout& KeyboardEntryActivity::currentLayout() const {
  const auto key = static_cast<uint16_t>(static_cast<uint16_t>(layoutId) << 4 | (shifted ? 1 : 0) | (symbols ? 2 : 0) |
                                         (urlPanel ? 4 : 0));
  // The snippet panel is a static table, not a build: nothing to cache.
  if (inputType == InputType::Url && urlPanel && !symbols) return URL_SNIPPET_LAYOUT;
  if (key != inputLayerKey) {
    buildLayer(inputLayer);
    inputLayerKey = key;
  }
  return inputLayer.layout;
}

const fui::KeyboardKey* KeyboardEntryActivity::selectedKey() const {
  const fui::KeyboardLayout& layout = currentLayout();
  if (selRow < 0 || selRow >= layout.rowCount) return nullptr;
  const fui::KeyboardRow& row = layout.rows[selRow];
  if (selCol < 0 || selCol >= row.count) return nullptr;
  return &row.keys[selCol];
}

int KeyboardEntryActivity::selectedLogicalIndex() const {
  const fui::KeyboardLayout& layout = currentLayout();
  int index = 0;
  for (int r = 0; r < selRow && r < layout.rowCount; r++) {
    index += layout.rows[r].count;
  }
  return index + selCol;
}

void KeyboardEntryActivity::clampSelection() {
  const fui::KeyboardLayout& layout = currentLayout();
  if (layout.rowCount == 0) {
    selRow = 0;
    selCol = 0;
    return;
  }
  if (selRow < 0) selRow = 0;
  if (selRow >= layout.rowCount) selRow = layout.rowCount - 1;
  const int cols = layout.rows[selRow].count;
  if (selCol < 0) selCol = 0;
  if (selCol >= cols) selCol = cols > 0 ? cols - 1 : 0;
}

void KeyboardEntryActivity::moveSelectionRow(const int delta) {
  const fui::KeyboardLayout& layout = currentLayout();
  if (layout.rowCount == 0) return;
  const int oldCols = selRow < layout.rowCount ? layout.rows[selRow].count : 1;
  selRow = (selRow + delta + layout.rowCount) % layout.rowCount;
  const int newCols = layout.rows[selRow].count;
  // Proportional column mapping keeps vertical travel intuitive between rows
  // of different key counts (e.g. a 10-key letter row over a 6-key bottom row).
  if (oldCols > 0 && newCols > 0 && oldCols != newCols) {
    selCol = selCol * newCols / oldCols;
  }
  clampSelection();
}

void KeyboardEntryActivity::moveSelectionCol(const int delta) {
  const fui::KeyboardLayout& layout = currentLayout();
  if (selRow < 0 || selRow >= layout.rowCount) return;
  const int cols = layout.rows[selRow].count;
  if (cols <= 0) return;
  selCol = (selCol + delta + cols) % cols;
}

bool KeyboardEntryActivity::syncSelectionToValue(const int16_t value) {
  const fui::KeyboardLayout& layout = currentLayout();
  for (int r = 0; r < layout.rowCount; r++) {
    for (int c = 0; c < layout.rows[r].count; c++) {
      if (layout.rows[r].keys[c].value == value) {
        selRow = r;
        selCol = c;
        return true;
      }
    }
  }
  return false;
}

size_t KeyboardEntryActivity::utf8Prev(const std::string& s, size_t pos) {
  if (pos == 0) return 0;
  pos--;
  while (pos > 0 && (static_cast<uint8_t>(s[pos]) & 0xC0) == 0x80) pos--;
  return pos;
}

size_t KeyboardEntryActivity::utf8Next(const std::string& s, size_t pos) {
  if (pos >= s.length()) return s.length();
  pos++;
  while (pos < s.length() && (static_cast<uint8_t>(s[pos]) & 0xC0) == 0x80) pos++;
  return pos;
}

void KeyboardEntryActivity::insertUtf8(const char* out) {
  if (!out || !*out) return;
  const size_t n = strlen(out);
  if (maxLength != 0 && text.length() + n > maxLength) return;
  if (cursorPos > text.length()) cursorPos = text.length();
  text.insert(cursorPos, out, n);
  cursorPos += n;
}

bool KeyboardEntryActivity::backspaceUtf8() {
  if (text.empty() || cursorPos == 0) return false;
  const size_t prev = utf8Prev(text, cursorPos);
  text.erase(prev, cursorPos - prev);
  cursorPos = prev;
  return true;
}

bool KeyboardEntryActivity::activateValue(const int16_t value, const bool longPress) {
  switch (value) {
    case fui::QWERTY_KEY_SHIFT:
      hintVisible = false;
      // Letters: case toggle. Symbols: page one or two.
      shifted = !shifted;
      clampSelection();
      return true;
    case fui::QWERTY_KEY_MODE:
      hintVisible = false;
      if (urlPanel) {
        urlPanel = false;
      } else {
        symbols = !symbols;
        shifted = false;
      }
      clampSelection();
      return true;
    case fui::QWERTY_KEY_LANG: {
      hintVisible = false;
      const fui::KeyboardLayoutId nextId = keyboard_layouts::next(layoutId);
      // The Cyrillic layers draw the key even with one layout enabled; a
      // full-screen e-ink repaint for an unchanged keyboard costs a second.
      if (nextId == layoutId) return false;
      layoutId = nextId;
      // Shift is per layer: carrying it across would strand the new layout in
      // upper case. Row widths differ between scripts (Cyrillic runs 12/11/11
      // against Latin's 10/9/9), so the selection has to be re-clamped.
      shifted = false;
      clampSelection();
      return true;
    }
    case URL_PANEL_KEY:
      hintVisible = false;
      urlPanel = !urlPanel;
      symbols = false;
      shifted = false;
      clampSelection();
      return true;
    case fui::QWERTY_KEY_ENTER:
      onComplete(text);
      return false;
    case fui::QWERTY_KEY_BACKSPACE:
      if (longPress) {
        text.clear();
        cursorPos = 0;
        return true;
      }
      backspaceUtf8();
      return true;
    default: {
      hintVisible = false;
      const fui::KeyboardLayout& layer = currentLayout();
      // keyboardAltOutputFor covers explicit alternates and the letter case flip.
      const char* out = longPress ? fui::keyboardAltOutputFor(layer, value) : nullptr;
      if (!out) out = fui::keyboardOutputFor(layer, value);
      if (!out) return false;
      insertUtf8(out);
      if (shifted && !symbols) {
        shifted = false;  // shift auto-releases after one character
        clampSelection();
      }
      return true;
    }
  }
}

bool KeyboardEntryActivity::clearAllOrAltOnSelected() {
  const fui::KeyboardKey* key = selectedKey();
  if (!key) return false;
  if (key->value == fui::QWERTY_KEY_BACKSPACE) {
    text.clear();
    cursorPos = 0;
    return true;
  }
  // Explicit alternates and the letter case flip, same as a touch long-press.
  const char* alt = fui::keyboardAltOutputFor(currentLayout(), key->value);
  if (alt) {
    insertUtf8(alt);
    return true;
  }
  return false;
}

std::string KeyboardEntryActivity::displayTextForCurrentState() const {
  std::string displayText = text;
  if (inputType != InputType::Password || passwordVisible) {
    return displayText;
  }

  // Masking is per byte so displayText keeps text's length and every index
  // means the same thing in both. A non-ASCII password character therefore
  // shows as two or three stars; passwords are rarely anything but ASCII.
  size_t revealPos;
  if (cursorMode) {
    revealPos = text.length();  // no reveal in displayText; the block draws the actual char
  } else {
    revealPos = (text.length() > 0 && cursorPos > 0) ? cursorPos - 1 : std::string::npos;
  }
  for (size_t i = 0; i < displayText.length(); i++) {
    if (i != revealPos) {
      displayText[i] = '*';
    }
  }
  return displayText;
}

int KeyboardEntryActivity::measureRange(std::string& s, const int start, const int end) const {
  if (end <= start) return 0;
  // s[end] is writable even at s.length() (the terminator slot); only '\0' may
  // be written there, which is exactly what the measurement needs.
  const char saved = s[end];
  s[end] = '\0';
  const int width = renderer.getTextAdvanceX(UI_12_FONT_ID, s.c_str() + start, EpdFontFamily::REGULAR);
  s[end] = saved;
  return width;
}

int KeyboardEntryActivity::lineBreakEnd(std::string& s, const int start, const int maxWidth) const {
  const int len = static_cast<int>(s.length());
  if (measureRange(s, start, len) <= maxWidth) return len;
  int lo = start + 1;
  int hi = len - 1;
  int best = start + 1;
  while (lo <= hi) {
    const int mid = lo + (hi - lo) / 2;
    if (measureRange(s, start, mid) <= maxWidth) {
      best = mid;
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }

  // The byte-index search can stop inside a character; snap back to a boundary,
  // keeping one whole character so the wrap loop always advances.
  const int firstCharEnd = static_cast<int>(utf8Next(s, static_cast<size_t>(start)));
  while (best > start && (static_cast<uint8_t>(s[best]) & 0xC0) == 0x80) best--;
  // Widths measured mid-character are unreliable, so the search can overshoot.
  while (best > firstCharEnd && measureRange(s, start, best) > maxWidth) {
    best = static_cast<int>(utf8Prev(s, static_cast<size_t>(best)));
  }
  return best < firstCharEnd ? firstCharEnd : best;
}

bool KeyboardEntryActivity::cursorPositionFromPoint(const int x, const int y, size_t& position) const {
  // Key taps are the overwhelmingly common case; they land on the keyboard,
  // never the text field, so skip the wrap/measure work entirely.
  if (y >= keyboardRect(currentLayout().rowCount).y) return false;

  const Rect content = keyboardContentRect(renderer);
  const auto& metrics = UITheme::getInstance().getMetrics();

  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int inputStartY = content.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing +
                          metrics.verticalSpacing * 4 + metrics.keyboardVerticalOffset;

  const int effectiveMargin = (content.width - content.width * metrics.keyboardTextFieldWidthPercent / 100) / 2;
  const int toggleGap = inputType == InputType::Password ? 4 : 0;
  const int toggleReserve = inputType == InputType::Password ? std::max(renderer.getTextWidth(UI_12_FONT_ID, "[abc]"),
                                                                        renderer.getTextWidth(UI_12_FONT_ID, "[***]")) +
                                                                   toggleGap
                                                             : 0;
  const int maxLineWidth = content.width - 2 * effectiveMargin - toggleReserve;
  const bool centerText = metrics.keyboardCenteredText;
  std::string displayText = displayTextForCurrentState();

  int lineStartIdx = 0;
  int lineY = inputStartY;
  int lastLineStartIdx = 0;
  int lastLineEndIdx = static_cast<int>(displayText.length());
  int lastLineStartX = content.x + effectiveMargin;
  int lastLineWidth = 0;

  while (true) {
    const int lineEndIdx = lineBreakEnd(displayText, lineStartIdx, maxLineWidth);
    const int textWidth = measureRange(displayText, lineStartIdx, lineEndIdx);
    const int lineStartX =
        content.x + (centerText ? effectiveMargin + (maxLineWidth - textWidth) / 2 : effectiveMargin);
    lastLineStartIdx = lineStartIdx;
    lastLineEndIdx = lineEndIdx;
    lastLineStartX = lineStartX;
    lastLineWidth = textWidth;

    if (y >= lineY - metrics.verticalSpacing && y < lineY + lineHeight + metrics.verticalSpacing) {
      if (x <= lineStartX) {
        position = static_cast<size_t>(lineStartIdx);
        return true;
      }
      if (x >= lineStartX + textWidth) {
        position = static_cast<size_t>(lineEndIdx);
        return true;
      }

      int previousWidth = 0;
      for (int i = lineStartIdx; i < lineEndIdx;) {
        const int next = static_cast<int>(utf8Next(displayText, static_cast<size_t>(i)));
        const int nextWidth = measureRange(displayText, lineStartIdx, next);
        const int midpoint = lineStartX + previousWidth + (nextWidth - previousWidth) / 2;
        if (x < midpoint) {
          position = static_cast<size_t>(i);
          return true;
        }
        previousWidth = nextWidth;
        i = next;
      }
      position = static_cast<size_t>(lineEndIdx);
      return true;
    }

    if (lineEndIdx == static_cast<int>(displayText.length())) {
      break;
    }

    lineY += lineHeight;
    lineStartIdx = lineEndIdx;
  }

  const int underlineBottom = lineY + lineHeight + metrics.verticalSpacing + 8;
  const int fieldLeft = content.x + effectiveMargin;
  if (y >= inputStartY - metrics.verticalSpacing && y < underlineBottom && x >= fieldLeft &&
      x < fieldLeft + maxLineWidth + toggleReserve) {
    position = static_cast<size_t>(x < lastLineStartX + lastLineWidth ? lastLineStartIdx : lastLineEndIdx);
    return true;
  }

  return false;
}

fui::Rect KeyboardEntryActivity::keyboardRect(const int rows) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect content = keyboardContentRect(renderer);
  const bool hasTouch = mappedInput.hasTouch();
  const int height = keyboardKeysHeight(metrics, rows, hasTouch);
  const int hintGap = hasTouch ? metrics.verticalSpacing - metrics.keyboardVerticalOffset : BUTTON_KEYBOARD_HINT_GAP;
  const int bottom = content.y + content.height;
  const int y = bottom - height - hintGap;
  return fui::Rect{static_cast<int16_t>(content.x), static_cast<int16_t>(y), static_cast<int16_t>(content.width),
                   static_cast<int16_t>(hasTouch ? bottom - y : height)};
}

void KeyboardEntryActivity::loop() {
  int tx = 0;
  int ty = 0;

  size_t touchedCursorPos = 0;
  if (mappedInput.wasScreenTapped(tx, ty) && cursorPositionFromPoint(tx, ty, touchedCursorPos)) {
    cursorPos = std::min(touchedCursorPos, text.length());
    // The masked text field maps taps per byte; snap back to a boundary so
    // the cursor never lands inside a multi-byte character.
    while (cursorPos > 0 && cursorPos < text.length() && (static_cast<uint8_t>(text[cursorPos]) & 0xC0) == 0x80) {
      cursorPos--;
    }
    cursorMode = false;
    togglePos = false;
    hintVisible = false;
    touchRouter.reset();
    requestUpdate();
    return;
  }

  if (!cursorMode && interactionsReady) {
    const bool pressedDown = mappedInput.wasScreenTouchDown(tx, ty);
    int tapX = 0;
    int tapY = 0;
    const bool tapped = mappedInput.wasScreenTapped(tapX, tapY);
    int hx = 0;
    int hy = 0;
    const bool inContact = mappedInput.isScreenTouchHeld(hx, hy);

    const fui::TouchHoldRouter::Result result =
        touchRouter.update(interactions, pressedDown, static_cast<int16_t>(tx), static_cast<int16_t>(ty), tapped,
                           static_cast<int16_t>(tapX), static_cast<int16_t>(tapY), inContact, millis());
    if (result.event) {
      syncSelectionToValue(result.event.value);
      if (activateValue(result.event.value, result.event.longPress)) {
        requestUpdate();
      }
      return;
    }
    if (result.activeChanged) {
      requestUpdate();
    }
    if (pressedDown || tapped) {
      return;
    }
  }

  using Direction = MappedInputManager::Direction;

  if (!cursorMode && mappedInput.wasLogicalPressed(Direction::Up)) {
    upHeld = true;
    upLongHandled = false;
  }

  if (upHeld && !upLongHandled && mappedInput.isLogicalPressed(Direction::Up) &&
      mappedInput.getHeldTime() > LONG_PRESS_MS) {
    cursorMode = true;
    upLongHandled = true;
    hintVisible = true;
    requestUpdate();
  }

  if (mappedInput.wasLogicalReleased(Direction::Up)) {
    if (upHeld && !upLongHandled && !cursorMode) {
      moveSelectionRow(-1);
      requestUpdate();
    }
    upHeld = false;
    upLongHandled = false;
  }

  if (mappedInput.wasLogicalPressed(Direction::Down)) {
    downHeld = true;
    if (cursorMode) {
      togglePos = false;
      passwordVisible = false;
      cursorMode = false;
      hintVisible = false;
      downLongHandled = true;
      requestUpdate();
    } else {
      downLongHandled = false;
    }
  }

  if (mappedInput.wasLogicalReleased(Direction::Down)) {
    if (downHeld && !downLongHandled && !cursorMode) {
      moveSelectionRow(1);
      requestUpdate();
    }
    downHeld = false;
    downLongHandled = false;
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::buttonFor(Direction::Left)}, [this] {
    if (cursorMode) return;
    moveSelectionCol(-1);
    requestUpdate();
  });

  if (mappedInput.wasLogicalReleased(Direction::Left)) {
    if (cursorMode) {
      if (togglePos) {
        cursorPos = savedCursorPos;
        togglePos = false;
        requestUpdate();
      } else if (cursorPos > 0) {
        cursorPos = utf8Prev(text, cursorPos);
        requestUpdate();
      }
    }
  }

  if (mappedInput.wasLogicalPressed(Direction::Right)) {
    if (cursorMode && inputType == InputType::Password && !togglePos) {
      rightHeld = true;
      rightLongHandled = false;
      rightStartCursorPos = cursorPos;
    }
  }

  buttonNavigator.onPressAndContinuous({MappedInputManager::buttonFor(Direction::Right)}, [this] {
    if (cursorMode) return;
    moveSelectionCol(1);
    requestUpdate();
  });

  if (rightHeld && !rightLongHandled && mappedInput.isLogicalPressed(Direction::Right) &&
      mappedInput.getHeldTime() > LONG_PRESS_MS) {
    if (cursorMode && inputType == InputType::Password && !togglePos) {
      savedCursorPos = rightStartCursorPos;
      togglePos = true;
      rightLongHandled = true;
      requestUpdate();
    }
  }

  if (mappedInput.wasLogicalReleased(Direction::Right)) {
    if (cursorMode && inputType == InputType::Password) {
      rightHeld = false;
      rightLongHandled = false;
    }
    if (cursorMode && !togglePos && cursorPos < text.length()) {
      cursorPos = utf8Next(text, cursorPos);
      requestUpdate();
    }
    if (cursorMode) return;
    rightHeld = false;
    rightLongHandled = false;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    confirmHeld = true;
    confirmLongHandled = false;
  }

  const fui::KeyboardKey* selKey = selectedKey();
  const bool selectedDel = selKey && selKey->value == fui::QWERTY_KEY_BACKSPACE;

  if (confirmHeld && !confirmLongHandled && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() > DEL_LONG_PRESS_MS && selectedDel) {
    clearAllOrAltOnSelected();
    confirmLongHandled = true;
    requestUpdate();
  }

  if (confirmHeld && !confirmLongHandled && mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() > LONG_PRESS_MS) {
    if (!selectedDel && clearAllOrAltOnSelected()) {
      requestUpdate();
      confirmLongHandled = true;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (confirmHeld && !confirmLongHandled && !cursorMode) {
      if (selKey && activateValue(selKey->value, false)) {
        requestUpdate();
      }
    } else if (confirmHeld && !confirmLongHandled && cursorMode && inputType == InputType::Password && togglePos) {
      passwordVisible = !passwordVisible;
      requestUpdate();
    }
    confirmHeld = false;
    confirmLongHandled = false;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onCancel();
  }
}

void KeyboardEntryActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const Rect content = keyboardContentRect(renderer);
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{content.x, metrics.topPadding, content.width, metrics.headerHeight}, title.c_str());

  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int inputStartY = content.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing +
                          metrics.verticalSpacing * 4 + metrics.keyboardVerticalOffset;
  int inputHeight = 0;

  std::string displayText = displayTextForCurrentState();

  const bool isPassword = (inputType == InputType::Password);
  const int effectiveMargin = (content.width - content.width * metrics.keyboardTextFieldWidthPercent / 100) / 2;
  const int toggleGap = isPassword ? 4 : 0;
  const int toggleReserve = isPassword ? std::max(renderer.getTextWidth(UI_12_FONT_ID, "[abc]"),
                                                  renderer.getTextWidth(UI_12_FONT_ID, "[***]")) +
                                             toggleGap
                                       : 0;
  const int maxLineWidth = content.width - 2 * effectiveMargin - toggleReserve;
  const bool centerText = metrics.keyboardCenteredText;

  // The cursor spans a whole code point: a lone byte of it renders as a replacement glyph.
  // Masking is per byte, so displayText keeps text's length and the same span applies to both.
  const size_t cursorCharBytes = (cursorPos < text.length()) ? utf8Next(text, cursorPos) - cursorPos : 0;
  char cursorChar[8] = {};         // the character under the cursor
  char displayCursorChar[8] = {};  // same span of displayText, masked for passwords
  if (cursorCharBytes > 0) {
    const size_t n = std::min(cursorCharBytes, sizeof(cursorChar) - 1);
    memcpy(cursorChar, text.data() + cursorPos, n);
    memcpy(displayCursorChar, displayText.data() + cursorPos, n);
  }

  int cursorCharWidth = 6;
  if (cursorCharBytes > 0) {
    int w = renderer.getTextWidth(UI_12_FONT_ID, cursorChar);
    if (w > cursorCharWidth) cursorCharWidth = w;
  }

  int lineStartIdx = 0;
  int textWidth = 0;
  int cursorPixelX = content.x + effectiveMargin;
  int cursorLineY = inputStartY;
  bool cursorDrawn = false;

  while (true) {
    const int lineEndIdx = lineBreakEnd(displayText, lineStartIdx, maxLineWidth);
    const std::string lineText = displayText.substr(lineStartIdx, lineEndIdx - lineStartIdx);
    textWidth = renderer.getTextAdvanceX(UI_12_FONT_ID, lineText.c_str(), EpdFontFamily::REGULAR);
    const int lineStartX =
        content.x + (centerText ? effectiveMargin + (maxLineWidth - textWidth) / 2 : effectiveMargin);
    const bool isLastLine = (lineEndIdx == static_cast<int>(displayText.length()));
    bool isCursorLine = false;
    if (!cursorDrawn && cursorPos >= static_cast<size_t>(lineStartIdx) &&
        (isLastLine ? cursorPos <= static_cast<size_t>(lineEndIdx) : cursorPos < static_cast<size_t>(lineEndIdx))) {
      std::string beforeCursor;
      if (isPassword && !passwordVisible && cursorMode) {
        beforeCursor = std::string(cursorPos - lineStartIdx, '*');
      } else {
        beforeCursor = displayText.substr(lineStartIdx, cursorPos - lineStartIdx);
      }
      const int beforeWidth = renderer.getTextAdvanceX(UI_12_FONT_ID, beforeCursor.c_str(), EpdFontFamily::REGULAR);
      int kernOffset = 0;
      if (cursorCharBytes > 0) {
        const std::string beforeAndCursor = beforeCursor + displayCursorChar;
        const int throughCursorWidth =
            renderer.getTextAdvanceX(UI_12_FONT_ID, beforeAndCursor.c_str(), EpdFontFamily::REGULAR);
        const int charAdvance = renderer.getTextAdvanceX(UI_12_FONT_ID, displayCursorChar, EpdFontFamily::REGULAR);
        kernOffset = throughCursorWidth - beforeWidth - charAdvance;
      }
      cursorPixelX = lineStartX + beforeWidth + kernOffset;
      cursorLineY = inputStartY + inputHeight;
      cursorDrawn = true;
      isCursorLine = true;
    }

    if (isCursorLine && cursorMode && isPassword && !passwordVisible && !togglePos) {
      // Draw text in 3 parts to avoid block cursor overflowing onto next char.
      // displayText uses '*' for all chars; actual char may be wider than '*'.
      // Part 1: chars before cursor position
      const std::string part1 = displayText.substr(lineStartIdx, cursorPos - lineStartIdx);
      renderer.drawText(UI_12_FONT_ID, lineStartX, inputStartY + inputHeight, part1.c_str());
      // Part 2: skip cursor slot (block + actual char drawn later)
      // Part 3: chars after cursor position (skip char under cursor), starting at cursorPixelX + cursorCharWidth
      const int afterStart = static_cast<int>(cursorPos + cursorCharBytes);
      const int afterEnd = lineEndIdx;
      if (afterStart < afterEnd) {
        const std::string part3 = displayText.substr(afterStart, afterEnd - afterStart);
        renderer.drawText(UI_12_FONT_ID, cursorPixelX + cursorCharWidth, inputStartY + inputHeight, part3.c_str());
      }
    } else {
      renderer.drawText(UI_12_FONT_ID, lineStartX, inputStartY + inputHeight, lineText.c_str());
    }
    if (isLastLine) {
      break;
    }

    inputHeight += lineHeight;
    lineStartIdx = lineEndIdx;
  }

  const int fieldWidth = (inputHeight > 0) ? maxLineWidth : textWidth;
  GUI.drawTextField(renderer, Rect{content.x, inputStartY, content.width, inputHeight}, fieldWidth, cursorMode,
                    effectiveMargin, content.width - 2 * effectiveMargin);

  if (cursorMode && !togglePos && cursorPos <= displayText.length()) {
    static constexpr int blockPadding = 1;
    renderer.fillRect(cursorPixelX - blockPadding, cursorLineY, cursorCharWidth + blockPadding * 2, lineHeight, true);
    if (cursorCharBytes > 0) {
      renderer.drawText(UI_12_FONT_ID, cursorPixelX, cursorLineY, cursorChar, false);
    }
  } else if (cursorPos <= displayText.length()) {
    static constexpr int serifW = 3;
    const int cX = cursorPixelX;
    const int cY = cursorLineY;
    const int cBottom = cursorLineY + lineHeight - 1;
    renderer.fillRect(cX, cY, 2, lineHeight, true);
    renderer.drawLine(cX - serifW, cY, cX - 1, cY, 2, true);
    renderer.drawLine(cX + 1, cY, cX + serifW, cY, 2, true);
    renderer.drawLine(cX - serifW, cBottom, cX - 1, cBottom, 2, true);
    renderer.drawLine(cX + 1, cBottom, cX + serifW, cBottom, 2, true);
  }

  if (isPassword) {
    const char* toggleLabel = passwordVisible ? "[***]" : "[abc]";
    const int toggleWidth = renderer.getTextWidth(UI_12_FONT_ID, toggleLabel);
    const int toggleX = content.x + content.width - effectiveMargin - toggleWidth;
    const int toggleY = inputStartY + inputHeight;
    const bool toggleSelected = cursorMode && togglePos;

    if (toggleSelected) {
      renderer.fillRect(toggleX - 2, toggleY, toggleWidth + 5, lineHeight + 3, true);
      renderer.drawText(UI_12_FONT_ID, toggleX, toggleY, toggleLabel, false);
    } else {
      renderer.drawText(UI_12_FONT_ID, toggleX, toggleY, toggleLabel, true);
    }
  }

  // The tips block below starts under these rows (its tipsTop) whether or not
  // they are drawn, so a row added here has to be reserved there as well.
  if (hintVisible && cursorMode && !text.empty()) {
    const int hintLh = renderer.getLineHeight(SMALL_FONT_ID);
    const int underlineY = inputStartY + inputHeight + lineHeight + metrics.verticalSpacing;
    const int hintY = underlineY + 4;
    int hintLineY = hintY;
    if (inputType == InputType::Password && togglePos) {
      renderer.drawCenteredText(
          SMALL_FONT_ID, hintLineY,
          passwordVisible ? tr(STR_KB_HINT_TOGGLE_HIDE_PASSWORD) : tr(STR_KB_HINT_TOGGLE_SHOW_PASSWORD), true);
      hintLineY += hintLh;
      renderer.drawCenteredText(SMALL_FONT_ID, hintLineY, tr(STR_KB_HINT_RETURN_CURSOR), true);
    } else {
      renderer.drawCenteredText(SMALL_FONT_ID, hintLineY, tr(STR_KB_HINT_MOVE_CURSOR), true);
      hintLineY += hintLh;
      if (inputType == InputType::Password) {
        const char* passTip = passwordVisible ? tr(STR_KB_HINT_HIDE_PASSWORD) : tr(STR_KB_HINT_SHOW_PASSWORD);
        renderer.drawCenteredText(SMALL_FONT_ID, hintLineY, passTip, true);
      }
    }
  }

  const fui::KeyboardLayout& layout = buildLayer(renderLayer);
  const bool hasTouch = mappedInput.hasTouch();
  const fui::Rect kbRect = keyboardRect(layout.rowCount);
  const int keysHeight = keyboardKeysHeight(metrics, layout.rowCount, hasTouch);
  const fui::Rect keysRect{kbRect.x, static_cast<int16_t>(kbRect.y + (kbRect.height - keysHeight) / 2), kbRect.width,
                           static_cast<int16_t>(keysHeight)};

  const int tipsLh = renderer.getLineHeight(SMALL_FONT_ID);
  const int underlineBottom = inputStartY + inputHeight + lineHeight + metrics.verticalSpacing + 4;
  auto drawTip = [&](const char* tip, int y) { renderer.drawCenteredText(SMALL_FONT_ID, y, tip, true); };

  // Ported from crosspoint-reader PR #3863 ("fix: stable tip positions in keyboard activity",
  // Uri Tauber / @Uri-Tauber). Theirs: the fixed row counts, the cursor-hint reservation, the
  // fit check and "Hold UP to edit entry" as a standing tip. Different here: the double-DEL
  // trigger that used to flash that tip for 4 s is gone too, with its timed repaint, which
  // upstream keeps although the repaint no longer changes anything on screen.
  //
  // Every row a mode can show counts, drawn or not: the Clear Text row is
  // blank while the field is empty, and counting it only once there was text
  // re-centred the whole block, visibly, on the first keystroke and again
  // whenever the field emptied.
  int tipCount = 0;
  if (cursorMode) {
    tipCount = 1;
  } else if (urlPanel) {
    tipCount = 3;
  } else if (symbols) {
    tipCount = 2;
  } else {
    tipCount = 3 + (inputType == InputType::Url ? 1 : 0);
  }
  const int tipsHeight = (tipCount + 1) * tipsLh;
  // In cursor mode the instructions above sit right under the field; start
  // below their rows, reserved even while an empty field hides them.
  const int tipsTop = underlineBottom + (cursorMode ? (isPassword ? 2 : 1) * tipsLh : 0);

  // Without the room (landscape, where the keys start just under the field,
  // or a long wrapped entry) the block would be drawn over the field and the
  // keys; leave it out instead.
  if (kbRect.y - tipsTop >= tipsHeight) {
    int y = tipsTop + (kbRect.y - tipsTop - tipsHeight) / 2;
    drawTip(tr(STR_KB_TIPS), y);
    y += tipsLh;
    if (!cursorMode) {
      drawTip(tr(STR_KB_HINT_EDIT_ENTRY), y);
      y += tipsLh;
    }
    if (cursorMode) {
      drawTip(tr(STR_KB_HINT_RETURN_KEYBOARD), y);
    } else if (urlPanel) {
      drawTip(tr(STR_KB_HINT_EXIT_URL_MODE), y);
      y += tipsLh;
      if (!text.empty()) {
        drawTip(tr(STR_KB_HINT_CLEAR_TEXT), y);
      }
    } else if (symbols) {
      if (!text.empty()) {
        drawTip(tr(STR_KB_HINT_CLEAR_TEXT), y);
      }
    } else {
      const char* altCharTip;
      if (inputType == InputType::Url) {
        altCharTip = tr(STR_KB_HINT_SECONDARY_CHAR);
      } else if (shifted) {
        altCharTip = tr(STR_KB_HINT_LOWER_SECONDARY);
      } else {
        altCharTip = tr(STR_KB_HINT_UPPER_SECONDARY);
      }
      drawTip(altCharTip, y);
      y += tipsLh;
      if (inputType == InputType::Url) {
        drawTip(tr(STR_KB_HINT_URL_SNIPPETS), y);
        y += tipsLh;
      }
      if (!text.empty()) {
        drawTip(tr(STR_KB_HINT_CLEAR_TEXT), y);
      }
    }
  }

  // The FreeInkUI keyboard draws the keys and registers their hit rects into
  // `interactions`; loop() routes touch against that table through
  // TouchHoldRouter, which reads the published generation. beginPublishCycle()
  // makes this render build into the OTHER generation, so loop() never sees a
  // half-rebuilt table whenever it runs relative to this.
  interactions.beginPublishCycle();
  fui::GfxRendererTarget target(renderer);
  target.setFont(fui::GfxRendererTarget::FONT_SMALL, SMALL_FONT_ID);
  target.setFont(fui::GfxRendererTarget::FONT_BODY, UI_12_FONT_ID);
  const fui::DeviceContext device = target.deviceContext();
  const fui::InputSnapshot noInput{};
  fui::Frame<56> frame(target, device, noInput, interactions);

  fui::KeyboardProps props;
  props.layout = &layout;
  props.keyAction = ACTION_KEY;  // one action id; loop() dispatches on the key value
  props.okLabel = tr(STR_OK_BUTTON);
  // Match the label to the layer the mode key leads back from: the symbols
  // layer and the URL snippet panel both lead back to "abc".
  props.modeLabel =
      (symbols || (inputType == InputType::Url && urlPanel)) ? tr(STR_KEY_MODE_ABC) : tr(STR_KEY_MODE_SYMBOLS);
  props.inputMask = static_cast<uint16_t>(fui::InputTouch | fui::InputLongPress);
  props.selectedIndex = (cursorMode || hasTouch) ? -1 : static_cast<int16_t>(selectedLogicalIndex());
  props.labelText.font = fui::GfxRendererTarget::FONT_BODY;
  props.altText.font = fui::GfxRendererTarget::FONT_SMALL;
  props.gap = props.rowGap = static_cast<int16_t>(keyboardGap(metrics));
  if (urlPanel) props.uniformKeyWidth = false;  // shortcut rows stretch independently
  if (!hasTouch) props.background = fui::Paint::none();
  frame.target().fill(kbRect, props.background);
  props.bottomHitOverflow = static_cast<int16_t>(std::max(0, kbRect.bottom() - keysRect.bottom()));
  fui::keyboard(frame, keysRect, props);
  interactions.publish();
  interactionsReady = true;

  const auto hints = mappedInput.mapHints(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT), ">", "<");
  GUI.drawButtonHints(renderer, hints.front.btn1, hints.front.btn2, hints.front.btn3, hints.front.btn4);
  GUI.drawSideButtonHints(renderer, hints.side.up, hints.side.down);

  renderer.displayBuffer();
}

void KeyboardEntryActivity::onComplete(std::string text) {
  setResult(KeyboardResult{std::move(text)});
  finish();
}

void KeyboardEntryActivity::onCancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}
