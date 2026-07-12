#include "SdEncryptionActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_random.h>
#include <mbedtls/aes.h>
#include <mbedtls/sha256.h>

#include <cstring>

#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

// ---------------------------------------------------------------------------
// Crypto helpers
// ---------------------------------------------------------------------------

void SdEncryptionActivity::deriveKey(const char* pin, uint8_t key[32]) {
  uint8_t hash[32];
  mbedtls_sha256(reinterpret_cast<const uint8_t*>(pin), strlen(pin), hash, 0);
  for (int i = 0; i < 1000; i++) {
    mbedtls_sha256(hash, 32, hash, 0);
  }
  memcpy(key, hash, 32);
  // Wipe intermediate from stack
  memset(hash, 0, sizeof(hash));
}

void SdEncryptionActivity::computeVerifyHash(const uint8_t key[32], uint8_t out[32]) {
  // Hash the 14-byte tag concatenated with the 32-byte key
  static constexpr uint8_t TAG[] = "BISCUIT_VERIFY";  // 14 bytes + null, use 14
  uint8_t buf[14 + 32];
  memcpy(buf, TAG, 14);
  memcpy(buf + 14, key, 32);
  mbedtls_sha256(buf, sizeof(buf), out, 0);
  memset(buf, 0, sizeof(buf));
}

void SdEncryptionActivity::saveVerifyToken(const uint8_t key[32]) {
  uint8_t token[32];
  computeVerifyHash(key, token);
  Storage.mkdir(BISCUIT_DIR);
  auto f = Storage.open(VERIFY_PATH, O_WRITE | O_CREAT | O_TRUNC);
  if (f) {
    f.write(token, 32);
    f.close();
  }
  memset(token, 0, sizeof(token));
}

bool SdEncryptionActivity::verifyPin(const uint8_t key[32]) const {
  auto f = Storage.open(VERIFY_PATH);
  if (!f) return false;  // no token yet — treat as "not yet encrypted"

  uint8_t stored[32];
  int r = f.read(stored, 32);
  f.close();
  if (r != 32) return false;

  uint8_t expected[32];
  computeVerifyHash(key, expected);

  // Constant-time compare
  volatile uint8_t diff = 0;
  for (int i = 0; i < 32; i++) diff |= stored[i] ^ expected[i];

  memset(stored, 0, sizeof(stored));
  memset(expected, 0, sizeof(expected));
  return diff == 0;
}

// ---------------------------------------------------------------------------
// File helpers
// ---------------------------------------------------------------------------

bool SdEncryptionActivity::isEligibleExt(const char* name) {
  // Find last '.'
  const char* dot = nullptr;
  for (const char* p = name; *p; p++) {
    if (*p == '.') dot = p;
  }
  if (!dot) return false;

  // Static table kept in flash
  static constexpr const char* EXT[] = {".dat", ".cfg", ".csv", ".json", ".txt"};
  for (const char* e : EXT) {
    if (strcmp(dot, e) == 0) return true;
  }
  return false;
}

int SdEncryptionActivity::countEligible(bool forEncrypt) const {
  int count = 0;
  HalFile dir = Storage.open(BISCUIT_DIR);
  if (!dir) return 0;

  HalFile entry;
  while ((entry = dir.openNextFile())) {
    if (entry.isDirectory()) { entry.close(); continue; }

    char name[128];
    entry.getName(name, sizeof(name));
    entry.close();

    if (name[0] == '\0' || name[0] == '.') continue;

    if (forEncrypt) {
      if (isEligibleExt(name)) count++;
    } else {
      // Count .benc files
      size_t len = strlen(name);
      if (len > 5 && strcmp(name + len - 5, ".benc") == 0) count++;
    }
  }
  dir.close();
  return count;
}

// ENC-004 helper: returns true if any .benc files exist in /biscuit/
bool SdEncryptionActivity::hasEncryptedFiles() const {
  HalFile dir = Storage.open(BISCUIT_DIR);
  if (!dir) return false;

  HalFile entry;
  while ((entry = dir.openNextFile())) {
    if (entry.isDirectory()) { entry.close(); continue; }

    char name[128];
    entry.getName(name, sizeof(name));
    entry.close();

    if (name[0] == '\0' || name[0] == '.') continue;

    size_t len = strlen(name);
    if (len > 5 && strcmp(name + len - 5, ".benc") == 0) {
      dir.close();
      return true;
    }
  }
  dir.close();
  return false;
}

// Encrypt a single file.  path must be an absolute path inside /biscuit/.
// Output is written to a temp file first; original is only deleted once the
// temp file has been fully flushed and closed (ENC-002, ENC-003, ENC-005).
bool SdEncryptionActivity::encryptFile(const char* path, const uint8_t key[32]) {
  // --- Read source ---
  HalFile src = Storage.open(path);
  if (!src) return false;

  uint32_t fileSize = static_cast<uint32_t>(src.size());
  if (fileSize > MAX_FILE_BYTES) { src.close(); return false; }

  // Allocate on heap — files are small (< 32 KB)
  uint32_t padded = ((fileSize + 15) / 16) * 16;
  if (padded == 0) padded = 16;  // at least one full block for PKCS7

  uint8_t* plain = static_cast<uint8_t*>(malloc(padded));
  if (!plain) { src.close(); return false; }
  memset(plain, 0, padded);

  int readBytes = src.read(plain, fileSize);
  src.close();
  if (readBytes < 0 || static_cast<uint32_t>(readBytes) != fileSize) {
    memset(plain, 0, padded);
    free(plain);
    return false;
  }

  // PKCS7 padding
  uint8_t padVal = static_cast<uint8_t>(padded - fileSize);
  for (uint32_t i = fileSize; i < padded; i++) plain[i] = padVal;

  // --- Generate IV ---
  uint8_t iv[IV_LEN];
  esp_fill_random(iv, IV_LEN);

  // --- Encrypt ---
  uint8_t* cipher = static_cast<uint8_t*>(malloc(padded));
  if (!cipher) {
    memset(plain, 0, padded);
    free(plain);
    return false;
  }

  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  if (mbedtls_aes_setkey_enc(&aes, key, 256) != 0) {
    mbedtls_aes_free(&aes);
    memset(plain, 0, padded);
    free(plain);
    free(cipher);
    return false;
  }

  uint8_t ivCopy[IV_LEN];
  memcpy(ivCopy, iv, IV_LEN);
  if (mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, padded, ivCopy, plain, cipher) != 0) {
    mbedtls_aes_free(&aes);
    memset(plain, 0, padded);
    free(plain);
    free(cipher);
    return false;
  }
  mbedtls_aes_free(&aes);

  memset(plain, 0, padded);
  free(plain);

  // --- Write to temp file first (ENC-002: atomic rename pattern) ---
  // Temp name: "<path>.benc.tmp"
  char tmpPath[264];
  snprintf(tmpPath, sizeof(tmpPath), "%s.benc.tmp", path);
  char outPath[264];
  snprintf(outPath, sizeof(outPath), "%s.benc", path);

  HalFile dst = Storage.open(tmpPath, O_WRITE | O_CREAT | O_TRUNC);
  if (!dst) {
    memset(cipher, 0, padded);
    free(cipher);
    return false;
  }

  // ENC-005: check every write return value
  bool writeOk =
      dst.write(reinterpret_cast<const uint8_t*>(MAGIC), MAGIC_LEN) == MAGIC_LEN &&
      dst.write(iv, IV_LEN) == IV_LEN &&
      dst.write(cipher, padded) == static_cast<int>(padded);

  dst.flush();
  dst.close();

  memset(cipher, 0, padded);
  free(cipher);

  // ENC-003: if any write failed, remove the partial temp file and bail
  if (!writeOk) {
    Storage.remove(tmpPath);
    return false;
  }

  // --- Atomically replace: delete original, rename temp → .benc (ENC-002) ---
  Storage.remove(path);
  Storage.rename(tmpPath, outPath);
  return true;
}

// Decrypt a single .benc file.  path points to the .benc file.
// Output is written to a temp file first; the .benc is only deleted once
// the temp file has been fully flushed and closed (ENC-002, ENC-003, ENC-005).
bool SdEncryptionActivity::decryptFile(const char* path, const uint8_t key[32]) {
  HalFile src = Storage.open(path);
  if (!src) return false;

  uint32_t fileSize = static_cast<uint32_t>(src.size());

  // Minimum: MAGIC(4) + IV(16) + at least one AES block(16)
  if (fileSize < static_cast<uint32_t>(MAGIC_LEN + IV_LEN + 16)) {
    src.close();
    return false;
  }

  uint32_t cipherLen = fileSize - MAGIC_LEN - IV_LEN;
  if (cipherLen > MAX_FILE_BYTES || (cipherLen % 16) != 0) {
    src.close();
    return false;
  }

  uint8_t* raw = static_cast<uint8_t*>(malloc(fileSize));
  if (!raw) { src.close(); return false; }

  int r = src.read(raw, fileSize);
  src.close();
  if (r < 0 || static_cast<uint32_t>(r) != fileSize) {
    memset(raw, 0, fileSize);
    free(raw);
    return false;
  }

  // Verify magic
  if (memcmp(raw, MAGIC, MAGIC_LEN) != 0) {
    memset(raw, 0, fileSize);
    free(raw);
    return false;
  }

  const uint8_t* iv      = raw + MAGIC_LEN;
  const uint8_t* cipher  = raw + MAGIC_LEN + IV_LEN;

  uint8_t* plain = static_cast<uint8_t*>(malloc(cipherLen));
  if (!plain) {
    memset(raw, 0, fileSize);
    free(raw);
    return false;
  }

  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  if (mbedtls_aes_setkey_dec(&aes, key, 256) != 0) {
    mbedtls_aes_free(&aes);
    memset(raw, 0, fileSize);
    free(raw);
    memset(plain, 0, cipherLen);
    free(plain);
    return false;
  }

  uint8_t ivCopy[IV_LEN];
  memcpy(ivCopy, iv, IV_LEN);
  if (mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, cipherLen, ivCopy, cipher, plain) != 0) {
    mbedtls_aes_free(&aes);
    memset(raw, 0, fileSize);
    free(raw);
    memset(plain, 0, cipherLen);
    free(plain);
    return false;
  }
  mbedtls_aes_free(&aes);
  memset(raw, 0, fileSize);
  free(raw);

  // Validate and strip PKCS7 padding
  uint8_t padVal2 = plain[cipherLen - 1];
  if (padVal2 == 0 || padVal2 > 16) {
    memset(plain, 0, cipherLen);
    free(plain);
    return false;
  }
  for (uint8_t i = 1; i <= padVal2; i++) {
    if (plain[cipherLen - i] != padVal2) {
      memset(plain, 0, cipherLen);
      free(plain);
      return false;
    }
  }
  uint32_t plainLen = cipherLen - padVal2;

  // Derive original filename: strip ".benc" suffix
  size_t pathLen = strlen(path);
  if (pathLen < 6) {  // ".benc" = 5 chars + at least one char before it
    memset(plain, 0, cipherLen);
    free(plain);
    return false;
  }
  char outPath[256];
  size_t copyLen = pathLen - 5;  // strip ".benc"
  if (copyLen >= sizeof(outPath) - 4) copyLen = sizeof(outPath) - 5;  // leave room for ".tmp"
  memcpy(outPath, path, copyLen);
  outPath[copyLen] = '\0';

  // --- Write to temp file first (ENC-002: atomic rename pattern) ---
  char tmpPath[264];
  snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", outPath);

  HalFile dst = Storage.open(tmpPath, O_WRITE | O_CREAT | O_TRUNC);
  if (!dst) {
    memset(plain, 0, cipherLen);
    free(plain);
    return false;
  }

  // ENC-005: check write return value
  bool writeOk = dst.write(plain, plainLen) == static_cast<int>(plainLen);
  dst.flush();
  dst.close();

  memset(plain, 0, cipherLen);
  free(plain);

  // ENC-003: if write failed, remove partial temp and bail (leave .benc intact)
  if (!writeOk) {
    Storage.remove(tmpPath);
    return false;
  }

  // --- Atomically replace: delete .benc, rename temp → original (ENC-002) ---
  Storage.remove(path);
  Storage.rename(tmpPath, outPath);
  return true;
}

bool SdEncryptionActivity::encryptAll(const uint8_t key[32]) {
  processedFiles = 0;
  hadError = false;

  HalFile dir = Storage.open(BISCUIT_DIR);
  if (!dir) {
    snprintf(errorMsg, sizeof(errorMsg), "Cannot open /biscuit/");
    hadError = true;
    return false;
  }

  // ENC-001: use class member fileNames[] instead of a local 4 KB stack array
  int count = 0;

  HalFile entry;
  while (count < MAX_FILES && (entry = dir.openNextFile())) {
    if (entry.isDirectory()) { entry.close(); continue; }

    char name[FNAME_MAX];
    entry.getName(name, sizeof(name));
    entry.close();

    if (name[0] == '\0' || name[0] == '.') continue;
    if (!isEligibleExt(name)) continue;

    snprintf(fileNames[count], FNAME_MAX, "%s", name);
    count++;
  }
  dir.close();

  totalFiles = count;

  for (int i = 0; i < count; i++) {
    char fullPath[256];
    snprintf(fullPath, sizeof(fullPath), "%s/%s", BISCUIT_DIR, fileNames[i]);

    if (!encryptFile(fullPath, key)) {
      snprintf(errorMsg, sizeof(errorMsg), "Failed: %.32s", fileNames[i]);
      hadError = true;
      // Continue with remaining files — partial encryption is still useful
    }
    processedFiles++;
    requestUpdate();  // refresh progress display
  }

  // Save verification token so we can check the PIN before decryption
  saveVerifyToken(key);
  return !hadError;
}

bool SdEncryptionActivity::decryptAll(const uint8_t key[32]) {
  processedFiles = 0;
  hadError = false;

  // Check PIN matches the stored token
  if (!verifyPin(key)) {
    snprintf(errorMsg, sizeof(errorMsg), "Wrong PIN");
    hadError = true;
    return false;
  }

  HalFile dir = Storage.open(BISCUIT_DIR);
  if (!dir) {
    snprintf(errorMsg, sizeof(errorMsg), "Cannot open /biscuit/");
    hadError = true;
    return false;
  }

  // ENC-001: use class member fileNames[] instead of a local 4 KB stack array
  int count = 0;

  HalFile entry;
  while (count < MAX_FILES && (entry = dir.openNextFile())) {
    if (entry.isDirectory()) { entry.close(); continue; }

    char name[FNAME_MAX];
    entry.getName(name, sizeof(name));
    entry.close();

    if (name[0] == '\0' || name[0] == '.') continue;

    size_t len = strlen(name);
    if (len <= 5 || strcmp(name + len - 5, ".benc") != 0) continue;

    snprintf(fileNames[count], FNAME_MAX, "%s", name);
    count++;
  }
  dir.close();

  totalFiles = count;

  for (int i = 0; i < count; i++) {
    char fullPath[256];
    snprintf(fullPath, sizeof(fullPath), "%s/%s", BISCUIT_DIR, fileNames[i]);

    if (!decryptFile(fullPath, key)) {
      snprintf(errorMsg, sizeof(errorMsg), "Failed: %.32s", fileNames[i]);
      hadError = true;
    }
    processedFiles++;
    requestUpdate();
  }

  // Remove verify token only if all decryptions succeeded
  if (!hadError) {
    Storage.remove(VERIFY_PATH);
  }
  return !hadError;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void SdEncryptionActivity::showMessage(const char* msg, unsigned long durationMs) {
  snprintf(msgBuf, sizeof(msgBuf), "%s", msg);
  msgUntilMs = millis() + durationMs;
  requestUpdate();
}

// ENC-004: phase 2 — user has verified current PIN; now ask for the new one
void SdEncryptionActivity::launchChangePinPhase2() {
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "Enter new PIN", "", 32, true),
      [this](const ActivityResult& result) {
        changePinPhase2 = false;

        if (result.isCancelled) {
          memset(oldKey, 0, sizeof(oldKey));
          return;
        }
        const auto& newPin = std::get<KeyboardResult>(result.data).text;
        if (newPin.empty()) {
          memset(oldKey, 0, sizeof(oldKey));
          showMessage("PIN cannot be empty");
          return;
        }

        uint8_t newKey[KEY_LEN];
        deriveKey(newPin.c_str(), newKey);
        saveVerifyToken(newKey);
        memset(newKey, 0, sizeof(newKey));
        memset(oldKey, 0, sizeof(oldKey));

        showMessage("PIN changed");
      });
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void SdEncryptionActivity::onEnter() {
  Activity::onEnter();
  state = MENU;
  menuIndex = 0;
  processedFiles = 0;
  totalFiles = 0;
  hadError = false;
  errorMsg[0] = '\0';
  msgBuf[0] = '\0';
  msgUntilMs = 0;
  changePinPhase2 = false;
  memset(oldKey, 0, sizeof(oldKey));
  requestUpdate();
}

void SdEncryptionActivity::onExit() {
  // Wipe any sensitive key material that might linger
  memset(oldKey, 0, sizeof(oldKey));
  Activity::onExit();
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------

void SdEncryptionActivity::loop() {
  // Clear timed message
  if (msgUntilMs && millis() >= msgUntilMs) {
    msgUntilMs = 0;
    msgBuf[0] = '\0';
    requestUpdate();
  }

  if (state == MENU) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finish();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      if (menuIndex > 0) { menuIndex--; requestUpdate(); }
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      if (menuIndex < MENU_ITEMS - 1) { menuIndex++; requestUpdate(); }
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      const int captured = menuIndex;

      if (captured == 2) {
        // ENC-004: Change PIN — two-phase flow.
        // Phase 1: verify current PIN.
        startActivityForResult(
            std::make_unique<KeyboardEntryActivity>(
                renderer, mappedInput, "Enter current PIN", "", 32, true),
            [this](const ActivityResult& result) {
              if (result.isCancelled) return;
              const auto& pin = std::get<KeyboardResult>(result.data).text;
              if (pin.empty()) return;

              uint8_t key[KEY_LEN];
              deriveKey(pin.c_str(), key);

              // Guard: refuse if encrypted files exist (they can't be re-keyed)
              if (hasEncryptedFiles()) {
                memset(key, 0, sizeof(key));
                showMessage("Decrypt files first");
                return;
              }

              // Verify token exists and matches
              if (Storage.exists(VERIFY_PATH) && !verifyPin(key)) {
                memset(key, 0, sizeof(key));
                showMessage("Wrong PIN");
                return;
              }

              // Correct PIN (or no token yet) — save key for phase 2
              memcpy(oldKey, key, KEY_LEN);
              memset(key, 0, sizeof(key));
              changePinPhase2 = true;

              // Phase 2: ask for the new PIN
              launchChangePinPhase2();
            });
        return;
      }

      // Encrypt (0) or Decrypt (1)
      startActivityForResult(
          std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "Enter PIN", "", 32, true),
          [this, captured](const ActivityResult& result) {
            if (result.isCancelled) return;
            const auto& pin = std::get<KeyboardResult>(result.data).text;
            if (pin.empty()) return;

            // Derive key on heap-friendly stack (256 bit = 32 bytes)
            uint8_t key[KEY_LEN];
            deriveKey(pin.c_str(), key);

            state = PROCESSING;
            hadError = false;
            errorMsg[0] = '\0';
            requestUpdate();

            bool ok = false;
            if (captured == 0) {
              // Encrypt
              lastOpEncrypt = true;
              ok = encryptAll(key);
            } else {
              // Decrypt
              lastOpEncrypt = false;
              ok = decryptAll(key);
            }
            (void)ok;

            // Wipe key from stack immediately
            memset(key, 0, sizeof(key));

            state = DONE;
            requestUpdate();
          });
    }
    return;
  }

  if (state == DONE) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      state = MENU;
      menuIndex = 0;
      processedFiles = 0;
      totalFiles = 0;
      hadError = false;
      errorMsg[0] = '\0';
      requestUpdate();
    }
    return;
  }

  // PROCESSING state: actual work is done inside the result handler (synchronously).
  // Loop just waits here while processing runs.
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void SdEncryptionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  switch (state) {
    case MENU:       renderMenu();       break;
    case PROCESSING: renderProcessing(); break;
    case DONE:       renderDone();       break;
  }

  renderer.displayBuffer();
}

void SdEncryptionActivity::renderMenu() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth  = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "SD Encryption");

  const int listTop    = metrics.topPadding + metrics.headerHeight;
  const int listHeight = pageHeight - listTop - metrics.buttonHintsHeight;

  static constexpr const char* LABELS[MENU_ITEMS] = {
      "Encrypt (Lock)",
      "Decrypt (Unlock)",
      "Change PIN",
  };
  static constexpr const char* SUBTITLES[MENU_ITEMS] = {
      "Encrypt .dat/.cfg/.csv/.json/.txt",
      "Decrypt previously encrypted files",
      "Update encryption PIN",
  };

  GUI.drawList(
      renderer,
      Rect{0, listTop, pageWidth, listHeight},
      MENU_ITEMS,
      menuIndex,
      [](int i) -> std::string { return LABELS[i]; },
      [](int i) -> std::string { return SUBTITLES[i]; });

  // Timed message overlay
  if (msgUntilMs && millis() < msgUntilMs) {
    GUI.drawPopup(renderer, msgBuf);
  }

  const auto labels = mappedInput.mapLabels("Back", "Select", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void SdEncryptionActivity::renderProcessing() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth  = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "SD Encryption");

  int y = pageHeight / 2 - 40;

  const char* verb = lastOpEncrypt ? "Encrypting" : "Decrypting";
  renderer.drawCenteredText(UI_12_FONT_ID, y, verb, true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(UI_12_FONT_ID) + 16;

  if (totalFiles > 0) {
    char buf[48];
    snprintf(buf, sizeof(buf), "%d / %d files", processedFiles, totalFiles);
    renderer.drawCenteredText(UI_10_FONT_ID, y, buf);
    y += renderer.getLineHeight(UI_10_FONT_ID) + 12;

    // Progress bar
    const int barW = pageWidth - 2 * metrics.contentSidePadding;
    const int barH = 14;
    const int barX = metrics.contentSidePadding;
    renderer.drawRect(barX, y, barW, barH, true);
    int fill = (totalFiles > 0)
        ? static_cast<int>((static_cast<long>(processedFiles) * (barW - 2)) / totalFiles)
        : 0;
    if (fill > 0) renderer.fillRect(barX + 1, y + 1, fill, barH - 2, true);
  } else {
    renderer.drawCenteredText(UI_10_FONT_ID, y, "Please wait...");
  }
}

void SdEncryptionActivity::renderDone() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth  = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "SD Encryption");

  int y = pageHeight / 2 - 60;

  if (hadError) {
    renderer.drawCenteredText(UI_12_FONT_ID, y, "Error", true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(UI_12_FONT_ID) + 12;
    renderer.drawCenteredText(UI_10_FONT_ID, y, errorMsg);
    y += renderer.getLineHeight(UI_10_FONT_ID) + 8;

    char buf[48];
    snprintf(buf, sizeof(buf), "%d / %d files processed", processedFiles, totalFiles);
    renderer.drawCenteredText(SMALL_FONT_ID, y, buf);
  } else {
    const char* doneLabel = lastOpEncrypt ? "Encrypted" : "Decrypted";
    renderer.drawCenteredText(UI_12_FONT_ID, y, doneLabel, true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(UI_12_FONT_ID) + 12;

    char buf[48];
    snprintf(buf, sizeof(buf), "%d file(s)", processedFiles);
    renderer.drawCenteredText(UI_10_FONT_ID, y, buf);
  }

  const auto labels = mappedInput.mapLabels("Back", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
