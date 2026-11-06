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

// v2: magic(4) version(1) keyLen(1) expiresAt(8, LE) | iv(12) | wrapped key
// (keyLen) | tag(16). The 14-byte header is the GCM additional data.
// v1 (read-only compat): 13-byte header without keyLen; key is 16 bytes.
constexpr uint8_t MAGIC[4] = {'F', 'I', 'B', 'K'};
constexpr uint8_t VERSION = 2;
constexpr size_t HEADER_LEN = 14;
constexpr size_t V1_HEADER_LEN = 13;
constexpr size_t IV_LEN = 12;
constexpr size_t TAG_LEN = 16;
constexpr size_t MAX_FILE_LEN = HEADER_LEN + IV_LEN + MAX_KEY_LEN + TAG_LEN;

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

bool write(const std::string& bookPath, const uint8_t* key, const size_t keyLen, const int64_t expiresAt) {
  if (keyLen != 16 && keyLen != 32) return false;
  uint8_t kek[32];
  if (!wrapKey(kek)) return false;

  const size_t fileLen = HEADER_LEN + IV_LEN + keyLen + TAG_LEN;
  uint8_t file[MAX_FILE_LEN];
  memcpy(file, MAGIC, sizeof(MAGIC));
  file[4] = VERSION;
  file[5] = static_cast<uint8_t>(keyLen);
  const uint64_t expires = static_cast<uint64_t>(expiresAt);
  for (int i = 0; i < 8; i++) file[6 + i] = static_cast<uint8_t>(expires >> (8 * i));
  uint8_t* iv = file + HEADER_LEN;
  esp_fill_random(iv, IV_LEN);
  uint8_t* wrapped = iv + IV_LEN;
  uint8_t* tag = wrapped + keyLen;

  AesGcm gcm;
  const bool sealed =
      gcm.ready && wc_AesGcmSetKey(gcm.aes.get(), kek, sizeof(kek)) == 0 &&
      wc_AesGcmEncrypt(gcm.aes.get(), wrapped, key, keyLen, iv, IV_LEN, tag, TAG_LEN, file, HEADER_LEN) == 0;
  if (!sealed) {
    LOG_ERR("BKEY", "Wrap failed");
    return false;
  }
  // Stage and swap: a torn write must not replace a working key with garbage.
  const std::string path = keyPath(bookPath);
  const std::string tmp = path + ".tmp";
  {
    HalFile f;
    if (!Storage.openFileForWrite("BKEY", tmp, f) || f.write(file, fileLen) != fileLen) {
      LOG_ERR("BKEY", "Write failed: %s", tmp.c_str());
      return false;
    }
  }
  return Storage.replaceFile(tmp.c_str(), path.c_str());
}

size_t read(const std::string& bookPath, uint8_t (&key)[MAX_KEY_LEN], int64_t* expiresAt) {
  uint8_t file[MAX_FILE_LEN];
  size_t got = 0;
  {
    HalFile f;
    if (!Storage.openFileForRead("BKEY", keyPath(bookPath), f)) return 0;
    const int n = f.read(file, sizeof(file));
    if (n <= 0) return 0;
    got = static_cast<size_t>(n);
  }
  if (got < V1_HEADER_LEN || memcmp(file, MAGIC, sizeof(MAGIC)) != 0) return 0;

  size_t headerLen = 0;
  size_t keyLen = 0;
  size_t expiresOffset = 0;
  if (file[4] == 1) {
    headerLen = V1_HEADER_LEN;
    keyLen = 16;
    expiresOffset = 5;
  } else if (file[4] == VERSION) {
    headerLen = HEADER_LEN;
    keyLen = file[5];
    expiresOffset = 6;
    if (keyLen != 16 && keyLen != 32) return 0;
  } else {
    return 0;
  }
  if (got != headerLen + IV_LEN + keyLen + TAG_LEN) return 0;

  uint8_t kek[32];
  if (!wrapKey(kek)) return 0;
  const uint8_t* iv = file + headerLen;
  const uint8_t* wrapped = iv + IV_LEN;
  const uint8_t* tag = wrapped + keyLen;
  AesGcm gcm;
  const bool opened =
      gcm.ready && wc_AesGcmSetKey(gcm.aes.get(), kek, sizeof(kek)) == 0 &&
      wc_AesGcmDecrypt(gcm.aes.get(), key, wrapped, keyLen, iv, IV_LEN, tag, TAG_LEN, file, headerLen) == 0;
  if (!opened) {
    LOG_ERR("BKEY", "Key not wrapped by this device or tampered: %s", bookPath.c_str());
    return 0;
  }
  uint64_t expires = 0;
  for (int i = 0; i < 8; i++) expires |= static_cast<uint64_t>(file[expiresOffset + i]) << (8 * i);
  *expiresAt = static_cast<int64_t>(expires);
  return keyLen;
}

}  // namespace bookkey
