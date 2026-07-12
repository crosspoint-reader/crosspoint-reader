#include "TotpActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <mbedtls/md.h>
#include <time.h>

#include <cstring>

#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

// ---- Base32 decode ----
int TotpActivity::base32Decode(const char* input, uint8_t* output, int outLen) {
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

uint32_t TotpActivity::generateTotp(const uint8_t* key, int keyLen, uint64_t counter, int digits) {
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
  uint32_t code = ((uint32_t)(hmac[offset] & 0x7F) << 24) | ((uint32_t)hmac[offset+1] << 16) |
                  ((uint32_t)hmac[offset+2] << 8) | hmac[offset+3];
  uint32_t mod = 1;
  for (int i = 0; i < digits; i++) mod *= 10;
  return code % mod;
}

uint32_t TotpActivity::currentCode() const {
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
void TotpActivity::loadAccounts() {
  accountCount = 0;
  auto file = Storage.open(SAVE_PATH);
  if (!file) return;
  file.read(reinterpret_cast<uint8_t*>(&accountCount), sizeof(accountCount));
  if (accountCount < 0 || accountCount > MAX_ACCOUNTS) { accountCount = 0; file.close(); return; }
  file.read(reinterpret_cast<uint8_t*>(accounts), sizeof(Account) * accountCount);
  file.close();
}

void TotpActivity::saveAccounts() {
  Storage.mkdir("/biscuit");
  auto file = Storage.open(SAVE_PATH, O_WRITE | O_CREAT | O_TRUNC);
  if (!file) return;
  file.write(reinterpret_cast<const uint8_t*>(&accountCount), sizeof(accountCount));
  file.write(reinterpret_cast<const uint8_t*>(accounts), sizeof(Account) * accountCount);
  file.close();
}

// ---- Lifecycle ----
void TotpActivity::onEnter() {
  Activity::onEnter();
  loadAccounts();
  state = ACCOUNT_LIST;
  selectedIndex = 0;
  requestUpdate();
}

void TotpActivity::onExit() { Activity::onExit(); }

// ---- Loop ----
void TotpActivity::loop() {
  if (state == ACCOUNT_LIST) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) { finish(); return; }
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      int total = accountCount + 1;
      selectedIndex = ButtonNavigator::previousIndex(selectedIndex, total);
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      int total = accountCount + 1;
      selectedIndex = ButtonNavigator::nextIndex(selectedIndex, total);
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (selectedIndex == accountCount) {
        // "Add Account"
        state = ADD_ACCOUNT;
        memset(pendingName, 0, sizeof(pendingName));
        startActivityForResult(
            std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "Account Name", "", 31),
            [this](const ActivityResult& r) {
              if (r.isCancelled) { state = ACCOUNT_LIST; return; }
              const auto& text = std::get<KeyboardResult>(r.data).text;
              strncpy(pendingName, text.c_str(), sizeof(pendingName) - 1);
              startActivityForResult(
                  std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "Base32 Secret", "", 64),
                  [this](const ActivityResult& r2) {
                    state = ACCOUNT_LIST;
                    if (r2.isCancelled) return;
                    if (accountCount >= MAX_ACCOUNTS) return;
                    const auto& sec = std::get<KeyboardResult>(r2.data).text;
                    Account& acc = accounts[accountCount++];
                    memset(&acc, 0, sizeof(acc));
                    strncpy(acc.name, pendingName, sizeof(acc.name) - 1);
                    strncpy(acc.secret, sec.c_str(), sizeof(acc.secret) - 1);
                    acc.digits = 6;
                    acc.period = 30;
                    saveAccounts();
                  });
            });
      } else {
        state = SHOW_CODE;
        requestUpdate();
      }
    }
    return;
  }

  if (state == SHOW_CODE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      state = ACCOUNT_LIST;
      requestUpdate();
      return;
    }
    // Auto-refresh once per second
    static unsigned long lastMs = 0;
    if (millis() - lastMs >= 1000) { lastMs = millis(); requestUpdate(); }
    return;
  }
}

// ---- Render ----
void TotpActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Authenticator");

  if (state == ACCOUNT_LIST) renderList();
  else if (state == SHOW_CODE) renderCode();

  renderer.displayBuffer();
}

void TotpActivity::renderList() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int listTop = metrics.topPadding + metrics.headerHeight;
  const int listH = pageHeight - listTop - metrics.buttonHintsHeight;
  const int total = accountCount + 1;

  GUI.drawList(renderer, Rect{0, listTop, pageWidth, listH}, total, selectedIndex,
    [this](int i) -> std::string {
      if (i < accountCount) return accounts[i].name;
      return "+ Add Account";
    });

  const auto labels = mappedInput.mapLabels("Back", "Select", "^", "v");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void TotpActivity::renderCode() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int top = metrics.topPadding + metrics.headerHeight + 20;

  if (selectedIndex < 0 || selectedIndex >= accountCount) return;

  renderer.drawCenteredText(UI_10_FONT_ID, top, accounts[selectedIndex].name);

  uint32_t code = currentCode();
  char codeBuf[16];
  snprintf(codeBuf, sizeof(codeBuf), "%06lu", (unsigned long)code);
  renderer.drawCenteredText(UI_12_FONT_ID, top + 50, codeBuf, true, EpdFontFamily::BOLD);

  // Progress bar (30-second countdown)
  time_t now = time(nullptr);
  int period = accounts[selectedIndex].period;
  int elapsed = (int)(now % period);
  int remaining = period - elapsed;
  const int barW = pageWidth - 80;
  const int barH = 12;
  const int barX = 40;
  const int barY = top + 110;
  renderer.drawRect(barX, barY, barW, barH, true);
  int fill = (barW * remaining) / period;
  if (fill > 0) renderer.fillRect(barX, barY, fill, barH, true);

  char timeBuf[16];
  snprintf(timeBuf, sizeof(timeBuf), "%ds", remaining);
  renderer.drawCenteredText(SMALL_FONT_ID, barY + barH + 6, timeBuf);

  const auto labels = mappedInput.mapLabels("Back", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
