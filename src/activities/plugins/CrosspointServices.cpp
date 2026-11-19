#if CROSSPOINT_DESSEN_APPS
#include "CrosspointServices.h"

#include <Crypto.h>         // freeink::content::Crypto, RsaKeyPairDer
#include <BookKey.h>
#include <DeviceSecret.h>
#include <HalFrontlight.h>  // declares the `Frontlight` singleton
#include <HalHaptics.h>
#include <HalPowerManager.h>  // declares the `powerManager` singleton
#include <HalStorage.h>
#include <WolfsslCrypto.h>  // freeink::content::WolfsslCrypto
#include <sdk/dessen_abi.h>
#include <time.h>
#include <wolfssl/wolfcrypt/aes.h>

#include <cstring>

#include "network/HttpDownloader.h"

using freeink::content::Crypto;
using freeink::content::RsaKeyPairDer;
using freeink::content::WolfsslCrypto;

namespace {
constexpr uint32_t kPathMax = 255;

// PKCS#7: pad to a 16-byte boundary, or validate+strip. Returns padded length
// (encrypt) / plaintext length (decrypt), or -1 on bad padding.
int pkcs7Strip(const uint8_t* buf, uint32_t len) {
  if (len == 0 || (len & 15)) return -1;
  const uint8_t pad = buf[len - 1];
  if (pad < 1 || pad > 16 || pad > len) return -1;
  for (uint32_t i = 0; i < pad; i++)
    if (buf[len - 1 - i] != pad) return -1;
  return static_cast<int>(len - pad);
}
}  // namespace

CrosspointServices::CrosspointServices(std::string appName, std::string appDir)
    : appName_(std::move(appName)), appDir_(std::move(appDir)), crypto_(new WolfsslCrypto()) {}
CrosspointServices::~CrosspointServices() = default;

// ---- core ----
uint32_t CrosspointServices::timeNow() {
  const time_t t = ::time(nullptr);
  return t > 0 ? static_cast<uint32_t>(t) : 0;
}

bool CrosspointServices::randomBytes(uint8_t* out, uint32_t n) {
  crypto_->randomBytes(out, n);
  return true;
}

// Per-app device id = hex(sha256(deviceSecret || appName)) — matches the web
// status route, and is unlinkable across apps.
int CrosspointServices::deviceId(char* out, uint32_t cap) {
  uint8_t secret[32];
  if (!deviceSecret(secret)) return DSN_ERR_UNAVAILABLE;
  uint8_t input[32 + 64];
  const size_t nameLen = appName_.size() > 64 ? 64 : appName_.size();
  memcpy(input, secret, 32);
  memcpy(input + 32, appName_.data(), nameLen);
  uint8_t hash[32];
  crypto_->sha256(input, 32 + nameLen, hash);
  if (65 > cap) return DSN_ERR_TOO_BIG;
  for (size_t i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", hash[i]);
  return 64;
}

// ---- storage ----
int CrosspointServices::resolve(const char* path, std::string& out) const {
  // The binding already enforced the path rules; map the app root to the SD
  // card. Apps are user-installed from the owner's own card (whole-card policy).
  if (!path || path[0] != '/' || strlen(path) > kPathMax) return DSN_ERR_INVALID;
  out = path;
  return 0;
}

int CrosspointServices::fsRead(const char* path, uint8_t* out, uint32_t cap) {
  std::string full;
  if (int e = resolve(path, full)) return e;
  HalFile f;
  if (!Storage.openFileForRead("DSN", full, f)) return DSN_ERR_IO;
  const size_t size = f.fileSize();
  if (size > cap) return DSN_ERR_TOO_BIG;
  const int got = f.read(out, size);
  return got == static_cast<int>(size) ? static_cast<int>(size) : DSN_ERR_IO;
}

int CrosspointServices::fsWrite(const char* path, const uint8_t* data, uint32_t n) {
  std::string full;
  if (int e = resolve(path, full)) return e;
  HalFile f;
  if (!Storage.openFileForWrite("DSN", full, f)) return DSN_ERR_IO;
  const size_t wrote = f.write(data, n);
  f.close();
  return wrote == n ? 0 : DSN_ERR_IO;
}

int CrosspointServices::fsList(const char* path, char* out, uint32_t cap) {
  std::string full;
  if (int e = resolve(path, full)) return e;
  HalFile dir;
  if (!Storage.openFileForRead("DSN", full, dir) || !dir.isDirectory()) return DSN_ERR_IO;
  std::string acc;
  for (HalFile e = dir.openNextFile(); e; e = dir.openNextFile()) {
    char name[256];
    if (e.getName(name, sizeof name) == 0) continue;
    acc += name;
    if (e.isDirectory()) acc += '/';
    acc += '\n';
  }
  if (acc.size() > cap) return DSN_ERR_TOO_BIG;
  memcpy(out, acc.data(), acc.size());
  return static_cast<int>(acc.size());
}

int CrosspointServices::fsDelete(const char* path) {
  std::string full;
  if (int e = resolve(path, full)) return e;
  return Storage.remove(full.c_str()) ? 0 : DSN_ERR_IO;
}

int CrosspointServices::fsAppDir(char* out, uint32_t cap) {
  if (appDir_.size() + 1 > cap) return DSN_ERR_TOO_BIG;
  memcpy(out, appDir_.c_str(), appDir_.size() + 1);
  return static_cast<int>(appDir_.size());
}

int CrosspointServices::keystore(const char* path, const uint8_t* key, uint32_t n, uint32_t expires) {
  std::string full;
  if (int e = resolve(path, full)) return e;
  if (n > bookkey::MAX_KEY_LEN) return DSN_ERR_INVALID;
  return bookkey::write(full, key, n, expires ? static_cast<int64_t>(expires) : 0) ? 0 : DSN_ERR_IO;
}

// ---- crypto ----
int CrosspointServices::hash(int algo, const uint8_t* data, uint32_t n, uint8_t* out, uint32_t cap) {
  if (algo == DSN_HASH_SHA1) {
    if (cap < 20) return DSN_ERR_TOO_BIG;
    crypto_->sha1(data, n, out);
    return 20;
  }
  if (algo == DSN_HASH_SHA256) {
    if (cap < 32) return DSN_ERR_TOO_BIG;
    crypto_->sha256(data, n, out);
    return 32;
  }
  return DSN_ERR_INVALID;
}

int CrosspointServices::aesCbc(int flags, const uint8_t* keyiv, const uint8_t* data, uint32_t n, uint8_t* out,
                               uint32_t cap) {
  const uint8_t* key = keyiv;
  const uint8_t* iv = keyiv + 16;
  const bool enc = flags & DSN_AES_ENCRYPT;
  const bool pad = !(flags & DSN_AES_NOPAD);
  if (enc) {
    // Build the (optionally padded) plaintext, then raw-encrypt in place.
    uint32_t len = n;
    if (pad) len = (n & ~15u) + 16;  // PKCS#7 always adds a full block when aligned
    if (len > cap) return DSN_ERR_TOO_BIG;
    if (out != data) memcpy(out, data, n);
    if (pad) memset(out + n, static_cast<uint8_t>(len - n), len - n);
    return crypto_->aes128CbcEncrypt(key, iv, out, len, out) ? static_cast<int>(len) : DSN_ERR_INVALID;
  }
  if (n > cap) return DSN_ERR_TOO_BIG;
  if (!crypto_->aes128CbcDecrypt(key, iv, data, n, out)) return DSN_ERR_INVALID;
  if (!pad) return static_cast<int>(n);
  const int stripped = pkcs7Strip(out, n);
  return stripped < 0 ? DSN_ERR_INVALID : stripped;
}

int CrosspointServices::rsaEncrypt(const uint8_t* cert, uint32_t cn, const uint8_t* data, uint32_t n, uint8_t* out,
                                   uint32_t cap) {
  size_t outLen = 0;
  if (!crypto_->rsaPublicEncrypt(cert, cn, data, n, out, cap, &outLen)) return DSN_ERR_INVALID;
  return static_cast<int>(outLen);
}

int CrosspointServices::rsaDecrypt(int flags, const uint8_t* priv, uint32_t pn, const uint8_t* data, uint32_t n,
                                   uint8_t* out, uint32_t cap) {
  uint8_t block[256];
  const int32_t blockLen = crypto_->rsaPrivateRaw(priv, pn, data, n, block, sizeof block);
  if (blockLen <= 0) return DSN_ERR_INVALID;
  if (flags & DSN_RSA_RAW) {
    if (static_cast<uint32_t>(blockLen) > cap) return DSN_ERR_TOO_BIG;
    memcpy(out, block, blockLen);
    return blockLen;
  }
  // Strip PKCS#1 v1.5 type-2: 00 02 <nonzero padding>=8 00 <message>.
  if (blockLen < 11 || block[0] != 0x00 || block[1] != 0x02) return DSN_ERR_INVALID;
  int sep = -1;
  for (int i = 2; i < blockLen; i++)
    if (block[i] == 0x00) { sep = i; break; }
  if (sep < 10) return DSN_ERR_INVALID;  // need >= 8 padding bytes
  const uint32_t msgLen = blockLen - sep - 1;
  if (msgLen > cap) return DSN_ERR_TOO_BIG;
  memcpy(out, block + sep + 1, msgLen);
  return static_cast<int>(msgLen);
}

int CrosspointServices::rsaSign(int algo, const uint8_t* priv, uint32_t pn, const uint8_t* digest, uint32_t dn,
                                uint8_t* out, uint32_t cap) {
  // The firmware primitive signs a 20-byte (SHA-1) digest, PKCS#1 v1.5, no
  // DigestInfo, into a 128-byte (1024-bit) signature — exactly the one case the
  // first consumer needs. Other hash/DigestInfo combinations are not provided.
  if ((algo & 0x7f) != DSN_HASH_SHA1 || !(algo & DSN_SIGN_NODIGESTINFO)) return DSN_ERR_INVALID;
  if (dn != 20 || cap < 128) return DSN_ERR_TOO_BIG;
  return crypto_->rsaPrivateSignRaw(priv, pn, digest, out) ? 128 : DSN_ERR_INVALID;
}

int CrosspointServices::pkcs12Parse(const uint8_t* p12, uint32_t n, const char* pass, uint8_t* out, uint32_t cap) {
  std::vector<uint8_t> key, cert;
  if (!crypto_->pkcs12Extract(p12, n, pass, &key, &cert)) return DSN_ERR_INVALID;
  if (key.size() >= 0x10000) return DSN_ERR_INVALID;
  const size_t total = 2 + key.size() + cert.size();
  if (total > cap) return DSN_ERR_TOO_BIG;
  out[0] = (key.size() >> 8) & 0xff;
  out[1] = key.size() & 0xff;
  memcpy(out + 2, key.data(), key.size());
  memcpy(out + 2 + key.size(), cert.data(), cert.size());
  return static_cast<int>(total);
}

bool CrosspointServices::deviceKek(uint8_t out[32]) const {
  uint8_t secret[32];
  if (!deviceSecret(secret)) return false;
  // Bind the wrap key to this app so another app's blobs will not unwrap.
  uint8_t input[32 + 64];
  const size_t nameLen = appName_.size() > 64 ? 64 : appName_.size();
  memcpy(input, secret, 32);
  memcpy(input + 32, appName_.data(), nameLen);
  crypto_->sha256(input, 32 + nameLen, out);
  return true;
}

int CrosspointServices::secureWrap(const uint8_t* data, uint32_t n, uint8_t* out, uint32_t cap) {
  if (2u + 12 + n + 16 > cap) return DSN_ERR_TOO_BIG;  // ver+reserved+nonce+ct+tag
  uint8_t kek[32];
  if (!deviceKek(kek)) return DSN_ERR_UNAVAILABLE;
  out[0] = 1;
  out[1] = 0;
  uint8_t* nonce = out + 2;
  crypto_->randomBytes(nonce, 12);
  uint8_t* ct = out + 14;
  Aes aes;
  if (wc_AesInit(&aes, nullptr, INVALID_DEVID) != 0) return DSN_ERR_IO;
  const bool ok = wc_AesGcmSetKey(&aes, kek, 32) == 0 &&
                  wc_AesGcmEncrypt(&aes, ct, data, n, nonce, 12, ct + n, 16, out, 2) == 0;
  wc_AesFree(&aes);
  return ok ? static_cast<int>(2 + 12 + n + 16) : DSN_ERR_IO;
}

int CrosspointServices::secureUnwrap(const uint8_t* blob, uint32_t n, uint8_t* out, uint32_t cap) {
  if (n < 2u + 12 + 16 || blob[0] != 1) return DSN_ERR_INVALID;
  const uint32_t ctn = n - 2 - 12 - 16;
  if (ctn > cap) return DSN_ERR_TOO_BIG;
  uint8_t kek[32];
  if (!deviceKek(kek)) return DSN_ERR_UNAVAILABLE;
  const uint8_t* nonce = blob + 2;
  const uint8_t* ct = blob + 14;
  Aes aes;
  if (wc_AesInit(&aes, nullptr, INVALID_DEVID) != 0) return DSN_ERR_IO;
  const bool ok = wc_AesGcmSetKey(&aes, kek, 32) == 0 &&
                  wc_AesGcmDecrypt(&aes, out, ct, ctn, nonce, 12, ct + ctn, 16, blob, 2) == 0;
  wc_AesFree(&aes);
  return ok ? static_cast<int>(ctn) : DSN_ERR_INVALID;  // auth failure -> INVALID
}

// ---- board ----
int CrosspointServices::backlightSet(int brightness, int warm) {
  if (!Frontlight.present()) return DSN_ERR_UNAVAILABLE;
  if (brightness >= 0) Frontlight.setBrightness(static_cast<uint8_t>(brightness > 100 ? 100 : brightness));
  if (warm >= 0 && Frontlight.hasColorTemperature()) Frontlight.setWarmth(static_cast<uint8_t>(warm > 100 ? 100 : warm));
  return 0;
}

int CrosspointServices::backlightGet() {
  if (!Frontlight.present()) return DSN_ERR_UNAVAILABLE;
  return Frontlight.brightness() | (Frontlight.hasColorTemperature() ? Frontlight.warmth() : 0) << 8;
}

int CrosspointServices::battery() {
  const uint16_t pct = powerManager.getBatteryPercentage();
  if (pct > 100) return DSN_ERR_UNAVAILABLE;
  return (pct & 0xff) | DSN_PWR_UNKNOWN << 8;  // charging state not exposed here
}

int CrosspointServices::haptic(int ms, int intensity) {
  (void)ms;  // HalHaptics plays a fixed short pattern; duration is advisory
  HalHaptics::longPress(true, static_cast<uint8_t>(intensity > 2 ? 2 : intensity < 0 ? 0 : intensity));
  return 0;
}

int CrosspointServices::accel(uint8_t*) {
  // The firmware tilt HAL exposes gyro rate, not acceleration in g. Until a
  // board surfaces raw accel, report "no such part".
  return DSN_ERR_UNAVAILABLE;
}

// ---- async: queue on the render task, execute on the loop task ----
int CrosspointServices::netStart(const char* head, const uint8_t* body, uint32_t bn, uint8_t* resp, uint32_t cap) {
  pending_ = Op::Net;
  done_ = false;
  head_ = head;
  body_.assign(body, body + bn);
  out_ = resp;
  outCap_ = cap;
  return dessen::kPending;
}

int CrosspointServices::fetchStart(const char* url, const char* dest) {
  pending_ = Op::Fetch;
  done_ = false;
  url_ = url;
  dest_ = dest;
  return dessen::kPending;
}

int CrosspointServices::keygenStart(uint32_t bits, uint8_t* pub, uint32_t pubCap, uint8_t* priv, uint32_t privCap) {
  pending_ = Op::Keygen;
  done_ = false;
  bits_ = bits;
  out_ = pub;
  outCap_ = pubCap;
  priv_ = priv;
  privCap_ = privCap;
  return dessen::kPending;
}

int CrosspointServices::textInputStart(const char* prompt, int flags, char* out, uint32_t cap) {
  pending_ = Op::TextInput;
  done_ = false;
  prompt_ = prompt;
  inputFlags_ = flags;
  out_ = reinterpret_cast<uint8_t*>(out);
  outCap_ = cap;
  return dessen::kPending;
}

int CrosspointServices::poll() {
  if (!done_) return dessen::kPending;
  pending_ = Op::None;
  return result_;
}

void CrosspointServices::serviceAsyncWork() {
  switch (pending_) {
    case Op::Net: {
      // head: first line "METHOD url", then "Header: value" lines.
      const size_t nl = head_.find('\n');
      std::string first = nl == std::string::npos ? head_ : head_.substr(0, nl);
      const size_t sp = first.find(' ');
      if (sp == std::string::npos) { result_ = DSN_ERR_INVALID; break; }
      const std::string method = first.substr(0, sp), url = first.substr(sp + 1);
      std::vector<HttpDownloader::Header> headers;
      size_t p = nl == std::string::npos ? head_.size() : nl + 1;
      while (p < head_.size()) {
        const size_t e = head_.find('\n', p);
        const std::string ln = head_.substr(p, (e == std::string::npos ? head_.size() : e) - p);
        const size_t c = ln.find(':');
        if (c != std::string::npos) headers.push_back({ln.substr(0, c), ln.substr(ln.find_first_not_of(' ', c + 1))});
        p = e == std::string::npos ? head_.size() : e + 1;
      }
      std::vector<uint8_t> bodyOut;
      HttpDownloader::FetchOptions opt;
      int status = 0;
      opt.statusOut = &status;
      opt.captureErrorBody = true;
      opt.headers = headers;
      const auto onData = [&](const uint8_t* d, size_t l) {
        bodyOut.insert(bodyOut.end(), d, d + l);
        return bodyOut.size() <= outCap_;  // stop if it will not fit
      };
      bool ok;
      if (method == "GET") {
        ok = HttpDownloader::fetchUrl(url, onData, opt);
      } else {  // POST/PUT: send body
        std::string resp;
        ok = HttpDownloader::postForm(url, std::string(body_.begin(), body_.end()), resp, &status);
        if (ok) bodyOut.assign(resp.begin(), resp.end());
      }
      if (!ok && status == 0) { result_ = DSN_ERR_IO; break; }
      if (4 + bodyOut.size() > outCap_) { result_ = DSN_ERR_TOO_BIG; break; }
      out_[0] = (status >> 8) & 0xff;
      out_[1] = status & 0xff;
      out_[2] = 0;  // header block length (not surfaced yet)
      out_[3] = 0;
      memcpy(out_ + 4, bodyOut.data(), bodyOut.size());
      result_ = static_cast<int>(bodyOut.size());
      break;
    }
    case Op::Fetch: {
      const HttpDownloader::DownloadError e = HttpDownloader::downloadToFile(url_, dest_);
      result_ = e == HttpDownloader::OK ? 0 : DSN_ERR_IO;
      break;
    }
    case Op::Keygen: {
      if (bits_ != 1024) { result_ = DSN_ERR_INVALID; break; }
      RsaKeyPairDer pair;
      if (!crypto_->rsaGenerate(&pair)) { result_ = DSN_ERR_IO; break; }
      if (pair.spki.size() > outCap_ || pair.pkcs8.size() > privCap_) { result_ = DSN_ERR_TOO_BIG; break; }
      memcpy(out_, pair.spki.data(), pair.spki.size());
      memcpy(priv_, pair.pkcs8.data(), pair.pkcs8.size());
      result_ = static_cast<int>(pair.spki.size()) << 16 | static_cast<int>(pair.pkcs8.size());
      break;
    }
    default:
      break;  // TextInput is completed by the activity via deliverText/cancelText
  }
  if (pending_ != Op::TextInput) done_ = true;
}

void CrosspointServices::deliverText(const char* text) {
  const size_t n = strlen(text);
  if (n + 1 > outCap_) {
    result_ = DSN_ERR_TOO_BIG;
  } else {
    memcpy(out_, text, n + 1);
    result_ = static_cast<int>(n);
  }
  done_ = true;
}

void CrosspointServices::cancelText() {
  result_ = -1;  // text_input returns -1 on cancel
  done_ = true;
}

#endif  // CROSSPOINT_DESSEN_APPS
