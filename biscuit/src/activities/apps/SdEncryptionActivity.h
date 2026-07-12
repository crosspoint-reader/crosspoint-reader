#pragma once
#include <cstdint>
#include "activities/Activity.h"

class SdEncryptionActivity final : public Activity {
 public:
  explicit SdEncryptionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("SdEncryption", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum State { MENU, PROCESSING, DONE };
  State state = MENU;

  // Menu
  int menuIndex = 0;
  static constexpr int MENU_ITEMS = 3;  // Encrypt, Decrypt, Change PIN

  // Progress tracking (set during blocking processing)
  int processedFiles = 0;
  int totalFiles = 0;
  bool lastOpEncrypt = true;   // true=encrypt, false=decrypt
  bool hadError = false;
  char errorMsg[48] = {};

  // Paths
  static constexpr const char* BISCUIT_DIR    = "/biscuit";
  static constexpr const char* VERIFY_PATH    = "/biscuit/.encrypt_verify";
  // Magic header written before IV+ciphertext
  static constexpr const char* MAGIC          = "BENC";
  static constexpr int         MAGIC_LEN      = 4;
  static constexpr int         IV_LEN         = 16;
  static constexpr int         KEY_LEN        = 32;
  static constexpr uint32_t    MAX_FILE_BYTES = 32 * 1024;

  // ENC-001: file name collection buffers moved to class members to avoid
  // 4 KB stack allocation inside result-handler lambda (deep call chain).
  static constexpr int MAX_FILES = 32;
  static constexpr int FNAME_MAX = 128;
  char fileNames[MAX_FILES][FNAME_MAX];  // ~4 KB, heap-allocated with the object

  // ENC-004: two-phase Change-PIN state
  bool    changePinPhase2 = false;
  uint8_t oldKey[KEY_LEN] = {};  // holds verified current-key during phase 1→2

  // Derive AES-256 key from PIN via iterated SHA-256
  static void deriveKey(const char* pin, uint8_t key[32]);

  // Compute verification hash: SHA-256("BISCUIT_VERIFY" || key) → 32 bytes
  static void computeVerifyHash(const uint8_t key[32], uint8_t out[32]);

  // Save / check the verification token on SD
  void saveVerifyToken(const uint8_t key[32]);
  bool verifyPin(const uint8_t key[32]) const;

  // Returns true if the file extension is eligible for encryption
  static bool isEligibleExt(const char* name);

  // Encrypt / decrypt all eligible files in /biscuit/.
  // key[32] must already be derived before calling.
  // Returns false and sets errorMsg on first hard failure.
  bool encryptAll(const uint8_t key[32]);
  bool decryptAll(const uint8_t key[32]);

  // Single-file helpers — return false on failure
  bool encryptFile(const char* path, const uint8_t key[32]);
  bool decryptFile(const char* path, const uint8_t key[32]);

  // Count eligible files (.dat/.cfg/.csv/.json/.txt) in /biscuit/
  int countEligible(bool forEncrypt) const;

  // Returns true if any .benc files exist in /biscuit/
  bool hasEncryptedFiles() const;

  // Timed message
  unsigned long msgUntilMs = 0;
  char msgBuf[48] = {};
  void showMessage(const char* msg, unsigned long durationMs = 1400);

  void renderMenu() const;
  void renderProcessing() const;
  void renderDone() const;

  // Internal: launch the "Enter new PIN" keyboard for phase 2 of Change PIN
  void launchChangePinPhase2();
};
