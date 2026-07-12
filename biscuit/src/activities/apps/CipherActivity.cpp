#include "CipherActivity.h"

#include <GfxRenderer.h>

#include <cstring>
#include <cstdlib>

#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

constexpr const char* CipherActivity::CIPHER_NAMES[];

// ---- Cipher implementations ----

std::string CipherActivity::rot13(const std::string& s) {
  return caesar(s, 13);
}

std::string CipherActivity::caesar(const std::string& s, int shift) {
  std::string out = s;
  shift = ((shift % 26) + 26) % 26;
  for (char& c : out) {
    if (c >= 'a' && c <= 'z') c = (char)('a' + (c - 'a' + shift) % 26);
    else if (c >= 'A' && c <= 'Z') c = (char)('A' + (c - 'A' + shift) % 26);
  }
  return out;
}

std::string CipherActivity::vigenere(const std::string& s, const std::string& key) {
  if (key.empty()) return s;
  std::string out = s;
  int ki = 0;
  const int klen = (int)key.size();
  for (char& c : out) {
    if (c >= 'a' && c <= 'z') {
      int k = (key[ki % klen] | 32) - 'a';
      c = (char)('a' + (c - 'a' + k) % 26);
      ki++;
    } else if (c >= 'A' && c <= 'Z') {
      int k = (key[ki % klen] | 32) - 'a';
      c = (char)('A' + (c - 'A' + k) % 26);
      ki++;
    }
  }
  return out;
}

std::string CipherActivity::xorCipher(const std::string& s, const std::string& key) {
  if (key.empty()) return s;
  std::string out = s;
  int klen = (int)key.size();
  for (int i = 0; i < (int)out.size(); i++) {
    out[i] ^= key[i % klen];
  }
  return out;
}

std::string CipherActivity::atbash(const std::string& s) {
  std::string out = s;
  for (char& c : out) {
    if (c >= 'a' && c <= 'z') c = (char)('z' - (c - 'a'));
    else if (c >= 'A' && c <= 'Z') c = (char)('Z' - (c - 'A'));
  }
  return out;
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string CipherActivity::base64Encode(const std::string& s) {
  std::string out;
  const uint8_t* d = reinterpret_cast<const uint8_t*>(s.data());
  int n = (int)s.size();
  for (int i = 0; i < n; i += 3) {
    uint32_t v = (uint32_t)d[i] << 16;
    if (i+1 < n) v |= (uint32_t)d[i+1] << 8;
    if (i+2 < n) v |= d[i+2];
    out += B64[(v >> 18) & 63];
    out += B64[(v >> 12) & 63];
    out += (i+1 < n) ? B64[(v >> 6) & 63] : '=';
    out += (i+2 < n) ? B64[v & 63]        : '=';
  }
  return out;
}

std::string CipherActivity::base64Decode(const std::string& s) {
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  std::string out;
  int buf = 0, bits = 0;
  for (char c : s) {
    int v = val(c);
    if (v < 0) continue;
    buf = (buf << 6) | v;
    bits += 6;
    if (bits >= 8) { bits -= 8; out += (char)((buf >> bits) & 0xFF); }
  }
  return out;
}

// ---- Activity ----

bool CipherActivity::needsKey() const {
  // Caesar, Vigenere, XOR need a key
  return cipherIndex == 1 || cipherIndex == 2 || cipherIndex == 3;
}

void CipherActivity::computeResult() {
  switch (cipherIndex) {
    case 0: result = rot13(inputText); break;
    case 1: result = caesar(inputText, atoi(keyText.c_str())); break;
    case 2: result = vigenere(inputText, keyText); break;
    case 3: result = xorCipher(inputText, keyText); break;
    case 4: result = atbash(inputText); break;
    case 5: result = base64Encode(inputText); break;
    case 6: result = base64Decode(inputText); break;
    default: result = inputText; break;
  }
}

void CipherActivity::onEnter() {
  Activity::onEnter();
  state = SELECT_CIPHER;
  cipherIndex = 0;
  inputText.clear();
  keyText.clear();
  result.clear();
  requestUpdate();
}

void CipherActivity::onExit() { Activity::onExit(); }

void CipherActivity::loop() {
  if (state == SELECT_CIPHER) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) { finish(); return; }
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      cipherIndex = ButtonNavigator::previousIndex(cipherIndex, CIPHER_COUNT);
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      cipherIndex = ButtonNavigator::nextIndex(cipherIndex, CIPHER_COUNT);
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      state = INPUT_TEXT;
      startActivityForResult(
          std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "Input Text", "", 200),
          [this](const ActivityResult& r) {
            if (r.isCancelled) { state = SELECT_CIPHER; return; }
            inputText = std::get<KeyboardResult>(r.data).text;
            if (needsKey()) {
              state = INPUT_KEY;
              const char* prompt = (cipherIndex == 1) ? "Shift (e.g. 3)" : "Key";
              startActivityForResult(
                  std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, prompt, "", 64),
                  [this](const ActivityResult& r2) {
                    if (r2.isCancelled) { state = SELECT_CIPHER; return; }
                    keyText = std::get<KeyboardResult>(r2.data).text;
                    computeResult();
                    state = RESULT;
                  });
            } else {
              keyText.clear();
              computeResult();
              state = RESULT;
            }
          });
    }
    return;
  }

  if (state == RESULT) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      state = SELECT_CIPHER; requestUpdate();
    }
    return;
  }
}

void CipherActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Cipher Tools");

  if (state == SELECT_CIPHER) renderSelect();
  else if (state == RESULT) renderResult();

  renderer.displayBuffer();
}

void CipherActivity::renderSelect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int listTop = metrics.topPadding + metrics.headerHeight;
  const int listH = pageHeight - listTop - metrics.buttonHintsHeight;

  GUI.drawList(renderer, Rect{0, listTop, pageWidth, listH}, CIPHER_COUNT, cipherIndex,
    [](int i) -> std::string { return CipherActivity::CIPHER_NAMES[i]; });

  const auto labels = mappedInput.mapLabels("Back", "Select", "^", "v");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void CipherActivity::renderResult() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int top = metrics.topPadding + metrics.headerHeight + 20;

  renderer.drawCenteredText(SMALL_FONT_ID, top, CIPHER_NAMES[cipherIndex]);
  renderer.drawCenteredText(SMALL_FONT_ID, top + 24, "Input:");
  renderer.drawCenteredText(UI_10_FONT_ID, top + 44, inputText.c_str());
  renderer.drawCenteredText(SMALL_FONT_ID, top + 90, "Result:");
  renderer.drawCenteredText(UI_10_FONT_ID, top + 110, result.c_str());

  const auto labels = mappedInput.mapLabels("Back", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
