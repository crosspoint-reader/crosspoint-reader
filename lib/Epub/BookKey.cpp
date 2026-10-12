#include "BookKey.h"

#include <DeviceSecret.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_random.h>
#include <wolfssl/wolfcrypt/aes.h>
#include <wolfssl/wolfcrypt/hash.h>

#include <cstring>
#include <memory>

namespace bookkey {

namespace {

// magic(4) version(1) expiresAt(8, LE) | iv(12) | wrapped key(16) | tag(16).
// The 13-byte header is the GCM additional data.
constexpr uint8_t MAGIC[4] = {'F', 'I', 'B', 'K'};
constexpr uint8_t VERSION = 1;
constexpr size_t HEADER_LEN = 13;
constexpr size_t IV_LEN = 12;
constexpr size_t TAG_LEN = 16;
constexpr size_t FILE_LEN = HEADER_LEN + IV_LEN + KEY_LEN + TAG_LEN;

std::string keyPath(const std::string& bookPath) { return bookPath + ".key"; }

// wolfSSL's Aes carries the GCM tables (KBs): heap, not the task stack.
struct AesGcm {
  std::unique_ptr<Aes> aes = makeUniqueNoThrow<Aes>();
  bool ready = aes && wc_AesInit(aes.get(), nullptr, INVALID_DEVID) == 0;
  ~AesGcm() {
    if (ready) wc_AesFree(aes.get());
  }
};

// sha256("book-key:" || secret): distinct from every plugin device ID, which
// hashes the secret first.
bool wrapKey(uint8_t (&out)[32]) {
  uint8_t secret[32];
  if (!deviceSecret(secret)) return false;
  static constexpr char LABEL[] = "book-key:";
  uint8_t input[sizeof(LABEL) - 1 + sizeof(secret)];
  memcpy(input, LABEL, sizeof(LABEL) - 1);
  memcpy(input + sizeof(LABEL) - 1, secret, sizeof(secret));
  return wc_Sha256Hash(input, sizeof(input), out) == 0;
}

}  // namespace

bool write(const std::string& bookPath, const uint8_t (&key)[KEY_LEN], const int64_t expiresAt) {
  uint8_t kek[32];
  if (!wrapKey(kek)) return false;

  uint8_t file[FILE_LEN];
  memcpy(file, MAGIC, sizeof(MAGIC));
  file[4] = VERSION;
  const uint64_t expires = static_cast<uint64_t>(expiresAt);
  for (int i = 0; i < 8; i++) file[5 + i] = static_cast<uint8_t>(expires >> (8 * i));
  uint8_t* iv = file + HEADER_LEN;
  esp_fill_random(iv, IV_LEN);
  uint8_t* wrapped = iv + IV_LEN;
  uint8_t* tag = wrapped + KEY_LEN;

  AesGcm gcm;
  const bool sealed =
      gcm.ready && wc_AesGcmSetKey(gcm.aes.get(), kek, sizeof(kek)) == 0 &&
      wc_AesGcmEncrypt(gcm.aes.get(), wrapped, key, KEY_LEN, iv, IV_LEN, tag, TAG_LEN, file, HEADER_LEN) == 0;
  if (!sealed) {
    LOG_ERR("BKEY", "Wrap failed");
    return false;
  }
  // Stage and swap: a torn write must not replace a working key with garbage.
  const std::string path = keyPath(bookPath);
  const std::string tmp = path + ".tmp";
  {
    HalFile f;
    if (!Storage.openFileForWrite("BKEY", tmp, f) || f.write(file, FILE_LEN) != FILE_LEN) {
      LOG_ERR("BKEY", "Write failed: %s", tmp.c_str());
      return false;
    }
  }
  return Storage.replaceFile(tmp.c_str(), path.c_str());
}

bool read(const std::string& bookPath, uint8_t (&key)[KEY_LEN], int64_t* expiresAt) {
  uint8_t file[FILE_LEN];
  size_t got = 0;
  {
    HalFile f;
    if (!Storage.openFileForRead("BKEY", keyPath(bookPath), f)) return false;
    const int n = f.read(file, sizeof(file));
    if (n <= 0) return false;
    got = static_cast<size_t>(n);
  }
  if (got != FILE_LEN || memcmp(file, MAGIC, sizeof(MAGIC)) != 0 || file[4] != VERSION) return false;

  uint8_t kek[32];
  if (!wrapKey(kek)) return false;
  const uint8_t* iv = file + HEADER_LEN;
  const uint8_t* wrapped = iv + IV_LEN;
  const uint8_t* tag = wrapped + KEY_LEN;
  AesGcm gcm;
  const bool opened =
      gcm.ready && wc_AesGcmSetKey(gcm.aes.get(), kek, sizeof(kek)) == 0 &&
      wc_AesGcmDecrypt(gcm.aes.get(), key, wrapped, KEY_LEN, iv, IV_LEN, tag, TAG_LEN, file, HEADER_LEN) == 0;
  if (!opened) {
    LOG_ERR("BKEY", "Key not wrapped by this device or tampered: %s", bookPath.c_str());
    return false;
  }
  uint64_t expires = 0;
  for (int i = 0; i < 8; i++) expires |= static_cast<uint64_t>(file[5 + i]) << (8 * i);
  *expiresAt = static_cast<int64_t>(expires);
  return true;
}

}  // namespace bookkey
