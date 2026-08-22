#include "TerminalTls.h"

#include <Preferences.h>
#include <esp_rom_crc.h>
#include <mbedtls/ecp.h>
#include <mbedtls/esp_mbedtls_random.h>
#include <mbedtls/md.h>
#include <mbedtls/oid.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509_crt.h>
#include <wolfssl/ssl.h>
#include <wolfssl/wolfcrypt/sha256.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace {

constexpr uint32_t IDENTITY_MAGIC = 0x4b544931;  // KTI1
constexpr uint32_t PAIRS_MAGIC = 0x4b545031;     // KTP1
constexpr uint16_t STORAGE_VERSION = 1;
constexpr char STORAGE_NAMESPACE[] = "knietty_tls";
constexpr char IDENTITY_KEY[] = "identity";
constexpr char PAIRS_KEY[] = "peers";
constexpr char SAS_DOMAIN[] = "knietty-pairing-v1";

uint32_t recordCrc(const void* record, const size_t lengthWithoutCrc) {
  return esp_rom_crc32_le(0xffffffffU, static_cast<const uint8_t*>(record), lengthWithoutCrc);
}

int tlsSend(WOLFSSL*, char* data, const int length, void* context) {
  auto* transport = static_cast<NetworkClient*>(context);
  const int written = transport->write(reinterpret_cast<const uint8_t*>(data), static_cast<size_t>(length));
  if (written > 0) return written;
  return transport->connected() ? WOLFSSL_CBIO_ERR_WANT_WRITE : WOLFSSL_CBIO_ERR_CONN_CLOSE;
}

int tlsReceive(WOLFSSL*, char* output, const int length, void* context) {
  auto* transport = static_cast<NetworkClient*>(context);
  if (transport->available() <= 0) {
    return transport->connected() ? WOLFSSL_CBIO_ERR_WANT_READ : WOLFSSL_CBIO_ERR_CONN_CLOSE;
  }
  const int read = transport->read(reinterpret_cast<uint8_t*>(output), static_cast<size_t>(length));
  return read > 0 ? read : WOLFSSL_CBIO_ERR_WANT_READ;
}

bool wantsIo(const int error) { return error == WOLFSSL_ERROR_WANT_READ || error == WOLFSSL_ERROR_WANT_WRITE; }

int acceptSelfSignedPeer(const int, WOLFSSL_X509_STORE_CTX*) {
  // Certificate chain policy is TOFU/pinning at the application layer. wolfSSL
  // still verifies CertificateVerify, so returning success here does not let a
  // peer authenticate without possession of the leaf private key.
  return 1;
}

void fingerprintText(const uint8_t* fingerprint, char* output, const size_t outputSize) {
  if (outputSize == 0) return;
  size_t offset = 0;
  for (size_t index = 0; index < 8 && offset + 3 < outputSize; ++index) {
    const int written =
        std::snprintf(output + offset, outputSize - offset, index == 0 ? "%02x" : ":%02x", fingerprint[index]);
    if (written <= 0) break;
    offset += static_cast<size_t>(written);
  }
  output[std::min(offset, outputSize - 1)] = '\0';
}

}  // namespace

TerminalTls::~TerminalTls() { end(); }

bool TerminalTls::generateIdentity(const char* hostname) {
  mbedtls_pk_context key;
  mbedtls_x509write_cert certificate;
  mbedtls_pk_init(&key);
  mbedtls_x509write_crt_init(&certificate);
  bool success = false;

  do {
    if (mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0) break;
    if (mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key), mbedtls_esp_random, nullptr) != 0) break;

    uint8_t serial[16];
    if (mbedtls_esp_random(nullptr, serial, sizeof(serial)) != 0) break;
    serial[0] &= 0x7f;
    serial[0] |= 0x01;

    char distinguishedName[64];
    const int nameLength = std::snprintf(distinguishedName, sizeof(distinguishedName), "CN=%s", hostname);
    if (nameLength <= 0 || nameLength >= static_cast<int>(sizeof(distinguishedName))) break;

    unsigned char serverAuthOid[] = MBEDTLS_OID_SERVER_AUTH;
    mbedtls_asn1_sequence extendedKeyUsage{};
    extendedKeyUsage.buf.tag = MBEDTLS_ASN1_OID;
    extendedKeyUsage.buf.len = MBEDTLS_OID_SIZE(MBEDTLS_OID_SERVER_AUTH);
    extendedKeyUsage.buf.p = serverAuthOid;

    mbedtls_x509write_crt_set_version(&certificate, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&certificate, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&certificate, &key);
    mbedtls_x509write_crt_set_issuer_key(&certificate, &key);
    if (mbedtls_x509write_crt_set_subject_name(&certificate, distinguishedName) != 0 ||
        mbedtls_x509write_crt_set_issuer_name(&certificate, distinguishedName) != 0 ||
        mbedtls_x509write_crt_set_serial_raw(&certificate, serial, sizeof(serial)) != 0 ||
        mbedtls_x509write_crt_set_validity(&certificate, "20260101000000", "20491231235959") != 0 ||
        mbedtls_x509write_crt_set_basic_constraints(&certificate, 0, -1) != 0 ||
        mbedtls_x509write_crt_set_key_usage(&certificate, MBEDTLS_X509_KU_DIGITAL_SIGNATURE) != 0 ||
        mbedtls_x509write_crt_set_ext_key_usage(&certificate, &extendedKeyUsage) != 0) {
      break;
    }

    identity = {};
    const int certificateLength = mbedtls_x509write_crt_der(&certificate, identity.certificate,
                                                            sizeof(identity.certificate), mbedtls_esp_random, nullptr);
    const int keyLength = mbedtls_pk_write_key_der(&key, identity.privateKey, sizeof(identity.privateKey));
    if (certificateLength <= 0 || keyLength <= 0) break;

    identity.magic = IDENTITY_MAGIC;
    identity.version = STORAGE_VERSION;
    identity.certificateLength = static_cast<uint16_t>(certificateLength);
    identity.privateKeyLength = static_cast<uint16_t>(keyLength);
    std::memmove(identity.certificate, identity.certificate + sizeof(identity.certificate) - certificateLength,
                 static_cast<size_t>(certificateLength));
    std::memmove(identity.privateKey, identity.privateKey + sizeof(identity.privateKey) - keyLength,
                 static_cast<size_t>(keyLength));
    identity.crc = recordCrc(&identity, offsetof(IdentityBlob, crc));

    Preferences preferences;
    if (!preferences.begin(STORAGE_NAMESPACE, false)) break;
    const size_t stored = preferences.putBytes(IDENTITY_KEY, &identity, sizeof(identity));
    preferences.end();
    success = stored == sizeof(identity);
  } while (false);

  mbedtls_x509write_crt_free(&certificate);
  mbedtls_pk_free(&key);
  return success;
}

bool TerminalTls::loadOrCreateIdentity(const char* hostname) {
  Preferences preferences;
  bool valid = false;
  if (preferences.begin(STORAGE_NAMESPACE, true)) {
    if (preferences.getBytesLength(IDENTITY_KEY) == sizeof(identity) &&
        preferences.getBytes(IDENTITY_KEY, &identity, sizeof(identity)) == sizeof(identity)) {
      valid = identity.magic == IDENTITY_MAGIC && identity.version == STORAGE_VERSION &&
              identity.certificateLength != 0 && identity.certificateLength <= CERTIFICATE_CAPACITY &&
              identity.privateKeyLength != 0 && identity.privateKeyLength <= PRIVATE_KEY_CAPACITY &&
              identity.crc == recordCrc(&identity, offsetof(IdentityBlob, crc));
    }
    preferences.end();
  }
  return valid || generateIdentity(hostname);
}

void TerminalTls::loadPairStore() {
  pairStore = {};
  Preferences preferences;
  bool valid = false;
  if (preferences.begin(STORAGE_NAMESPACE, true)) {
    if (preferences.getBytesLength(PAIRS_KEY) == sizeof(pairStore) &&
        preferences.getBytes(PAIRS_KEY, &pairStore, sizeof(pairStore)) == sizeof(pairStore)) {
      valid = pairStore.magic == PAIRS_MAGIC && pairStore.version == STORAGE_VERSION &&
              pairStore.count <= MAX_PAIRED_HOSTS && pairStore.crc == recordCrc(&pairStore, offsetof(PairStore, crc));
    }
    preferences.end();
  }
  if (valid) return;
  pairStore = {};
  pairStore.magic = PAIRS_MAGIC;
  pairStore.version = STORAGE_VERSION;
}

bool TerminalTls::commitPairStore(PairStore& candidate) {
  candidate.crc = recordCrc(&candidate, offsetof(PairStore, crc));
  Preferences preferences;
  if (!preferences.begin(STORAGE_NAMESPACE, false)) return false;
  const size_t stored = preferences.putBytes(PAIRS_KEY, &candidate, sizeof(candidate));
  preferences.end();
  if (stored != sizeof(candidate)) return false;
  pairStore = candidate;
  return true;
}

bool TerminalTls::begin(const char* hostname) {
  end();
  if (hostname == nullptr || hostname[0] == '\0' || !loadOrCreateIdentity(hostname)) return false;
  loadPairStore();

  auto* tlsContext = wolfSSL_CTX_new(wolfTLSv1_3_server_method());
  if (tlsContext == nullptr) return false;
  context = tlsContext;
  if (wolfSSL_CTX_use_certificate_buffer(tlsContext, identity.certificate, identity.certificateLength,
                                         WOLFSSL_FILETYPE_ASN1) != WOLFSSL_SUCCESS ||
      wolfSSL_CTX_use_PrivateKey_buffer(tlsContext, identity.privateKey, identity.privateKeyLength,
                                        WOLFSSL_FILETYPE_ASN1) != WOLFSSL_SUCCESS) {
    end();
    return false;
  }
  wolfSSL_CTX_set_verify(tlsContext, WOLFSSL_VERIFY_PEER | WOLFSSL_VERIFY_FAIL_IF_NO_PEER_CERT, acceptSelfSignedPeer);
  wolfSSL_SetIORecv(tlsContext, tlsReceive);
  wolfSSL_SetIOSend(tlsContext, tlsSend);

  uint8_t fingerprint[FINGERPRINT_SIZE];
  if (wc_Sha256Hash(identity.certificate, identity.certificateLength, fingerprint) != 0) {
    end();
    return false;
  }
  fingerprintText(fingerprint, deviceFingerprint, sizeof(deviceFingerprint));
  ready = true;
  return true;
}

void TerminalTls::end() {
  stop();
  if (context != nullptr) {
    wolfSSL_CTX_free(static_cast<WOLFSSL_CTX*>(context));
    context = nullptr;
  }
  ready = false;
}

bool TerminalTls::attach(NetworkClient&& incoming) {
  stop();
  if (!ready || !incoming.connected()) return false;
  transport = incoming;
  transport.setNoDelay(true);
  auto* tlsSession = wolfSSL_new(static_cast<WOLFSSL_CTX*>(context));
  if (tlsSession == nullptr) {
    transport.stop();
    return false;
  }
  session = tlsSession;
  wolfSSL_SetIOReadCtx(tlsSession, &transport);
  wolfSSL_SetIOWriteCtx(tlsSession, &transport);
#ifdef HAVE_MAX_FRAGMENT
  wolfSSL_UseMaxFragment(tlsSession, WOLFSSL_MFL_2_11);
#endif
  handshakeStartedAt = millis();
  handshakeMinHeap = ESP.getFreeHeap();
  return true;
}

TerminalTls::HandshakeResult TerminalTls::pollHandshake() {
  if (session == nullptr || !transport.connected()) return HandshakeResult::Failed;
  handshakeMinHeap = std::min(handshakeMinHeap, ESP.getFreeHeap());
  auto* tlsSession = static_cast<WOLFSSL*>(session);
  const int result = wolfSSL_accept(tlsSession);
  if (result == WOLFSSL_SUCCESS) {
    handshakeMs = millis() - handshakeStartedAt;
    return capturePeerIdentity() ? HandshakeResult::Connected : HandshakeResult::Failed;
  }
  const int error = wolfSSL_get_error(tlsSession, result);
  return wantsIo(error) ? HandshakeResult::Pending : HandshakeResult::Failed;
}

bool TerminalTls::capturePeerIdentity() {
  auto* certificate = wolfSSL_get_peer_certificate(static_cast<WOLFSSL*>(session));
  if (certificate == nullptr) return false;
  int length = 0;
  const unsigned char* der = wolfSSL_X509_get_der(certificate, &length);
  const int hashed =
      der != nullptr && length > 0 ? wc_Sha256Hash(der, static_cast<size_t>(length), peerFingerprint) : -1;
  wolfSSL_X509_free(certificate);
  if (hashed != 0) return false;
  peerReady = true;
  peerPaired = false;
  for (uint8_t index = 0; index < pairStore.count; ++index) {
    if (std::memcmp(pairStore.hosts[index].fingerprint, peerFingerprint, FINGERPRINT_SIZE) == 0) {
      peerPaired = true;
      break;
    }
  }
  updatePairingCode();
  return true;
}

void TerminalTls::updatePairingCode() {
  uint8_t deviceHash[FINGERPRINT_SIZE];
  uint8_t input[sizeof(SAS_DOMAIN) - 1 + FINGERPRINT_SIZE * 2];
  wc_Sha256Hash(identity.certificate, identity.certificateLength, deviceHash);
  std::memcpy(input, SAS_DOMAIN, sizeof(SAS_DOMAIN) - 1);
  std::memcpy(input + sizeof(SAS_DOMAIN) - 1, deviceHash, FINGERPRINT_SIZE);
  std::memcpy(input + sizeof(SAS_DOMAIN) - 1 + FINGERPRINT_SIZE, peerFingerprint, FINGERPRINT_SIZE);
  uint8_t digest[FINGERPRINT_SIZE];
  wc_Sha256Hash(input, sizeof(input), digest);
  const uint32_t value = (static_cast<uint32_t>(digest[0]) << 24 | static_cast<uint32_t>(digest[1]) << 16 |
                          static_cast<uint32_t>(digest[2]) << 8 | digest[3]) %
                         1000000U;
  std::snprintf(sasCode, sizeof(sasCode), "%06lu", static_cast<unsigned long>(value));
}

bool TerminalTls::peerNameChanged(const char* name) const {
  if (!peerReady || name == nullptr) return false;
  for (uint8_t index = 0; index < pairStore.count; ++index) {
    if (std::strncmp(pairStore.hosts[index].name, name, HOST_NAME_SIZE) == 0 &&
        std::memcmp(pairStore.hosts[index].fingerprint, peerFingerprint, FINGERPRINT_SIZE) != 0) {
      return true;
    }
  }
  return false;
}

bool TerminalTls::trustPeer(const char* name) {
  if (!peerReady || name == nullptr || name[0] == '\0') return false;
  if (peerPaired) return true;
  if (pairStore.count >= MAX_PAIRED_HOSTS) return false;
  stagedPairStore = pairStore;
  PairedHost& host = stagedPairStore.hosts[stagedPairStore.count];
  std::memcpy(host.fingerprint, peerFingerprint, FINGERPRINT_SIZE);
  std::snprintf(host.name, sizeof(host.name), "%s", name);
  ++stagedPairStore.count;
  if (!commitPairStore(stagedPairStore)) return false;
  peerPaired = true;
  return true;
}

const char* TerminalTls::pairedPeerName(const uint8_t index) const {
  return index < pairStore.count ? pairStore.hosts[index].name : "";
}

bool TerminalTls::formatPairedPeerFingerprint(const uint8_t index, char* output, const size_t outputSize) const {
  if (index >= pairStore.count || output == nullptr || outputSize == 0) return false;
  fingerprintText(pairStore.hosts[index].fingerprint, output, outputSize);
  return true;
}

bool TerminalTls::forgetPeer(const uint8_t index) {
  if (index >= pairStore.count) return false;
  stagedPairStore = pairStore;
  for (uint8_t current = index; current + 1 < stagedPairStore.count; ++current) {
    stagedPairStore.hosts[current] = stagedPairStore.hosts[current + 1];
  }
  --stagedPairStore.count;
  stagedPairStore.hosts[stagedPairStore.count] = {};
  return commitPairStore(stagedPairStore);
}

bool TerminalTls::forgetAllPeers() {
  stagedPairStore = {};
  stagedPairStore.magic = PAIRS_MAGIC;
  stagedPairStore.version = STORAGE_VERSION;
  if (!commitPairStore(stagedPairStore)) return false;
  peerPaired = false;
  return true;
}

int TerminalTls::available() {
  if (!connected()) return 0;
  return wolfSSL_pending(static_cast<WOLFSSL*>(session)) + transport.available();
}

int TerminalTls::read() {
  uint8_t byte = 0;
  return read(&byte, 1) == 1 ? byte : -1;
}

int TerminalTls::read(uint8_t* output, const size_t length) {
  if (!connected() || output == nullptr || length == 0) return -1;
  auto* tlsSession = static_cast<WOLFSSL*>(session);
  const int result = wolfSSL_read(tlsSession, output, static_cast<int>(length));
  if (result > 0) return result;
  const int error = wolfSSL_get_error(tlsSession, result);
  if (wantsIo(error)) return 0;
  stop();
  return -1;
}

size_t TerminalTls::write(const uint8_t* data, const size_t length) {
  if (!connected() || data == nullptr || length == 0) return 0;
  auto* tlsSession = static_cast<WOLFSSL*>(session);
  const int result = wolfSSL_write(tlsSession, data, static_cast<int>(length));
  if (result > 0) return static_cast<size_t>(result);
  if (!wantsIo(wolfSSL_get_error(tlsSession, result))) stop();
  return 0;
}

bool TerminalTls::connected() { return session != nullptr && transport.connected(); }

void TerminalTls::stop() {
  if (session != nullptr) {
    wolfSSL_free(static_cast<WOLFSSL*>(session));
    session = nullptr;
  }
  if (transport) transport.stop();
  transport = NetworkClient{};
  std::memset(peerFingerprint, 0, sizeof(peerFingerprint));
  sasCode[0] = '\0';
  peerReady = false;
  peerPaired = false;
}
