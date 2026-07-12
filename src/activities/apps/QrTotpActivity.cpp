#include "QrTotpActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <mbedtls/md.h>
#include <time.h>

#include <cstring>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ButtonNavigator.h"
#include "util/QrUtils.h"

// ---- TOTP helpers (same logic as TotpActivity, self-contained) ----

int QrTotpActivity::base32Decode(const char* input, uint8_t* output, int outLen) {
  static const char B32[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
  int bits = 0, val = 0, idx = 0;
  for (int i = 0; input[i] && idx < outLen; i++) {
    char c = input[i];
    if (c >= 'a' && c <= 'z') c -= 32;
    const char* p = strchr(B32, c);
    if (!p) continue;
    val = (val << 5) | (int)(p - B32);
    bits += 5;
    if (bits >= 8) {
      output[idx++] = (uint8_t)(val >> (bits - 8));
      bits -= 8;
    }
  }
  return idx;
}

uint32_t QrTotpActivity::generateTotp(const uint8_t* key, int keyLen, uint64_t counter, int digits) {
  uint8_t msg[8];
  for (int i = 7; i >= 0; i--) { msg[i] = counter & 0xFF; counter >>= 8; }
  uint8_t hmac[20];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), 1);
  mbedtls_md_hmac_starts(&ctx, key, keyLen);
  mbedtls_md_hmac_update(&ctx, msg, 8);
  mbedtls_md_hmac_finish(&ctx, hmac);
  mbedtls_md_free(&ctx);
  int offset = hmac[19] & 0x0F;
  uint32_t code = ((uint32_t)(hmac[offset] & 0x7F) << 24) | ((uint32_t)hmac[offset + 1] << 16) |
                  ((uint32_t)hmac[offset + 2] << 8) | hmac[offset + 3];
  uint32_t mod = 1;
  for (int i = 0; i < digits; i++) mod *= 10;
  return code % mod;
}

uint32_t QrTotpActivity::currentCode() const {
  if (!timeValid) return 0;
  if (selectedIndex < 0 || selectedIndex >= accountCount) return 0;
  const Account& acc = accounts[selectedIndex];
  uint8_t key[40];
  int keyLen = base32Decode(acc.secret, key, sizeof(key));
  if (keyLen <= 0) return 0;
  uint64_t t = (uint64_t)time(nullptr) / acc.period;
  uint32_t code = generateTotp(key, keyLen, t, acc.digits);
  volatile uint8_t* vkey = key;
  for (size_t i = 0; i < sizeof(key); i++) vkey[i] = 0;
  return code;
}

// ---- Storage ----

void QrTotpActivity::loadAccounts() {
  accountCount = 0;
  auto file = Storage.open(SAVE_PATH);
  if (!file) return;
  file.read(reinterpret_cast<uint8_t*>(&accountCount), sizeof(accountCount));
  if (accountCount < 0 || accountCount > MAX_ACCOUNTS) { accountCount = 0; file.close(); return; }
  file.read(reinterpret_cast<uint8_t*>(accounts), sizeof(Account) * accountCount);
  file.close();
}

// ---- Lifecycle ----

void QrTotpActivity::onEnter() {
  Activity::onEnter();
  loadAccounts();
  // No battery-backed RTC: after a cold boot time(nullptr) returns ~0 (epoch
  // 1970). Require the clock to be plausibly in the post-2020 range before
  // trusting it for TOTP generation.
  timeValid = (time(nullptr) > 1600000000);
  state = SELECT_ACCOUNT;
  selectedIndex = 0;
  lastPeriodIndex = 0;
  requestUpdate();
}

void QrTotpActivity::onExit() { Activity::onExit(); }

// ---- Loop ----

void QrTotpActivity::loop() {
  if (state == SELECT_ACCOUNT) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) { finish(); return; }
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      selectedIndex = ButtonNavigator::previousIndex(selectedIndex, accountCount);
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      selectedIndex = ButtonNavigator::nextIndex(selectedIndex, accountCount);
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (accountCount > 0) {
        state = SHOW_QR;
        lastPeriodIndex = 0;
        requestUpdate();
      }
    }
    return;
  }

  if (state == SHOW_QR) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      state = SELECT_ACCOUNT;
      requestUpdate();
      return;
    }
    // Auto-refresh when the 30-second TOTP period rolls over.
    // Only meaningful when the clock is valid — skip entirely on cold boot.
    if (timeValid && selectedIndex >= 0 && selectedIndex < accountCount) {
      uint8_t period = accounts[selectedIndex].period;
      uint64_t currentPeriod = (uint64_t)time(nullptr) / period;
      if (currentPeriod != lastPeriodIndex) {
        lastPeriodIndex = currentPeriod;
        requestUpdate();
      }
    }
    return;
  }
}

// ---- Render ----

void QrTotpActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "TOTP QR");

  if (state == SELECT_ACCOUNT) {
    if (accountCount == 0) {
      const int midY = pageHeight / 2;
      renderer.drawCenteredText(UI_10_FONT_ID, midY - 12, "No accounts found.");
      renderer.drawCenteredText(SMALL_FONT_ID, midY + 12, "Add accounts in Authenticator first.");
    } else {
      const int listTop = metrics.topPadding + metrics.headerHeight;
      const int listH = pageHeight - listTop - metrics.buttonHintsHeight;
      GUI.drawList(renderer, Rect{0, listTop, pageWidth, listH}, accountCount, selectedIndex,
          [this](int i) -> std::string { return accounts[i].name; });
    }

    const auto labels = mappedInput.mapLabels("Back", accountCount > 0 ? "Select" : "", "^", "v");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  } else {
    // SHOW_QR
    if (selectedIndex < 0 || selectedIndex >= accountCount) {
      renderer.displayBuffer();
      return;
    }

    const auto labels = mappedInput.mapLabels("Back", "", "", "");

    if (!timeValid) {
      // Clock has not been synchronised — show a warning instead of a wrong code.
      const int midY = pageHeight / 2;
      renderer.drawCenteredText(UI_12_FONT_ID, midY - 24, "Clock not set", true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, midY + 4, "Connect to WiFi to sync time.");
      renderer.drawCenteredText(SMALL_FONT_ID, midY + 28, "TOTP codes require a valid clock.");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      renderer.displayBuffer();
      return;
    }

    const Account& acc = accounts[selectedIndex];

    // Generate numeric code string
    uint32_t code = currentCode();
    char codeBuf[10];
    snprintf(codeBuf, sizeof(codeBuf), "%0*lu", (int)acc.digits, (unsigned long)code);

    // Layout: QR in the upper portion, text info below
    const int headerBottom = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int hintsTop = pageHeight - metrics.buttonHintsHeight;
    const int textAreaH = 60;  // reserved at bottom for code text + countdown
    const int qrAreaH = hintsTop - headerBottom - textAreaH - metrics.verticalSpacing;
    const int qrAreaW = pageWidth - 40;

    const Rect qrBounds(20, headerBottom, qrAreaW, qrAreaH);
    QrUtils::drawQrCode(renderer, qrBounds, std::string(codeBuf));

    // Account name above hints
    const int textY = hintsTop - textAreaH;
    renderer.drawCenteredText(UI_10_FONT_ID, textY, acc.name);

    // Bold code digits
    renderer.drawCenteredText(UI_12_FONT_ID, textY + 22, codeBuf, true, EpdFontFamily::BOLD);

    // Countdown
    time_t now = time(nullptr);
    int period = acc.period;
    int remaining = period - (int)(now % period);
    char timeBuf[8];
    snprintf(timeBuf, sizeof(timeBuf), "%ds", remaining);
    renderer.drawCenteredText(SMALL_FONT_ID, textY + 46, timeBuf);

    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
