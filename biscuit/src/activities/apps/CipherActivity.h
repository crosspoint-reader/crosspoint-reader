#pragma once
#include <string>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class CipherActivity final : public Activity {
 public:
  explicit CipherActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Cipher", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum State { SELECT_CIPHER, INPUT_TEXT, INPUT_KEY, RESULT };
  State state = SELECT_CIPHER;

  static constexpr int CIPHER_COUNT = 7;
  static constexpr const char* CIPHER_NAMES[CIPHER_COUNT] = {
    "ROT13", "Caesar", "Vigenere", "XOR", "Atbash", "Base64 Encode", "Base64 Decode"
  };

  int cipherIndex = 0;
  std::string inputText;
  std::string keyText;
  std::string result;

  bool needsKey() const;
  void computeResult();
  void renderSelect() const;
  void renderResult() const;

  // Cipher implementations
  static std::string rot13(const std::string& s);
  static std::string caesar(const std::string& s, int shift);
  static std::string vigenere(const std::string& s, const std::string& key);
  static std::string xorCipher(const std::string& s, const std::string& key);
  static std::string atbash(const std::string& s);
  static std::string base64Encode(const std::string& s);
  static std::string base64Decode(const std::string& s);
};
