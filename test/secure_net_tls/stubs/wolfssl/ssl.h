#pragma once

// Exercise the production transport's verification setup and failure cleanup.
// Certificate cryptography itself belongs to wolfSSL and device verification.
#include <cstddef>
#include <string>

struct WOLFSSL_METHOD {};
struct WOLFSSL_CTX {};
struct WOLFSSL {};
inline constexpr int WOLFSSL_SUCCESS = 1, WOLFSSL_VERIFY_NONE = 0, WOLFSSL_VERIFY_PEER = 1;
inline constexpr int WOLFSSL_FILETYPE_PEM = 1, WOLFSSL_SNI_HOST_NAME = 0;
inline constexpr int WOLFSSL_CBIO_ERR_CONN_CLOSE = -1, WOLFSSL_CBIO_ERR_WANT_WRITE = -2,
                     WOLFSSL_CBIO_ERR_WANT_READ = -3;
inline constexpr int WOLFSSL_ERROR_WANT_READ = 2, WOLFSSL_ERROR_WANT_WRITE = 3, WOLFSSL_ERROR_ZERO_RETURN = 6;

namespace FakeTls {
inline int caResult = 1, domainResult = 1, handshakeResult = 1;
inline int verifyMode = -1, contexts = 0, sessions = 0, handshakes = 0, writes = 0;
inline std::string hostname;
}  // namespace FakeTls

inline WOLFSSL_METHOD* wolfSSLv23_client_method() {
  static WOLFSSL_METHOD m;
  return &m;
}
inline WOLFSSL_METHOD* wolfTLSv1_2_client_method() { return wolfSSLv23_client_method(); }
inline WOLFSSL_CTX* wolfSSL_CTX_new(WOLFSSL_METHOD*) {
  ++FakeTls::contexts;
  return new WOLFSSL_CTX;
}
inline void wolfSSL_CTX_free(WOLFSSL_CTX* ctx) {
  --FakeTls::contexts;
  delete ctx;
}
inline void wolfSSL_CTX_set_verify(WOLFSSL_CTX*, int mode, void*) { FakeTls::verifyMode = mode; }
inline int wolfSSL_CTX_load_verify_buffer(WOLFSSL_CTX*, const unsigned char*, long, int) { return FakeTls::caResult; }
inline void wolfSSL_SetIORecv(WOLFSSL_CTX*, int (*)(WOLFSSL*, char*, int, void*)) {}
inline void wolfSSL_SetIOSend(WOLFSSL_CTX*, int (*)(WOLFSSL*, char*, int, void*)) {}
inline WOLFSSL* wolfSSL_new(WOLFSSL_CTX*) {
  ++FakeTls::sessions;
  return new WOLFSSL;
}
inline void wolfSSL_free(WOLFSSL* ssl) {
  --FakeTls::sessions;
  delete ssl;
}
inline int wolfSSL_check_domain_name(WOLFSSL*, const char* host) {
  FakeTls::hostname = host;
  return FakeTls::domainResult;
}
inline void wolfSSL_SetIOReadCtx(WOLFSSL*, void*) {}
inline void wolfSSL_SetIOWriteCtx(WOLFSSL*, void*) {}
inline int wolfSSL_UseSNI(WOLFSSL*, int, const void*, unsigned short) { return 1; }
inline int wolfSSL_connect(WOLFSSL*) {
  ++FakeTls::handshakes;
  return FakeTls::handshakeResult;
}
inline int wolfSSL_get_error(WOLFSSL*, int) { return -188; }
inline const char* wolfSSL_get_version(WOLFSSL*) { return "fake"; }
inline const char* wolfSSL_get_cipher(WOLFSSL*) { return "fake"; }
inline int wolfSSL_write(WOLFSSL*, const void*, int n) {
  ++FakeTls::writes;
  return n;
}
inline int wolfSSL_read(WOLFSSL*, void*, int) { return -1; }
inline int wolfSSL_pending(WOLFSSL*) { return 0; }
