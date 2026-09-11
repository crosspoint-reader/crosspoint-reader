#pragma once

#include <gtest/gtest.h>
#include <openssl/evp.h>

#include <cstring>
#include <string>

#include "Fixture.h"
#include "Logging.h"

// Host adapter for the Arduino MD5Builder surface used by the real document-ID
// implementation. OpenSSL supplies the independent digest, not production code.
class MD5Builder {
 public:
  MD5Builder() {
    if (!injectedFailure(documentIdFixture::Md5Operation::Context)) context = EVP_MD_CTX_new();
    if (!context) {
      fail("EVP_MD_CTX_new failed");
    } else {
      ++documentIdFixture::state.md5ContextsCreated;
    }
  }
  ~MD5Builder() {
    if (context) {
      EVP_MD_CTX_free(context);
      ++documentIdFixture::state.md5ContextsFreed;
    }
  }
  MD5Builder(const MD5Builder&) = delete;
  MD5Builder& operator=(const MD5Builder&) = delete;

  void begin() {
    if (failed) return;
    if (injectedFailure(documentIdFixture::Md5Operation::Initialize) ||
        EVP_DigestInit_ex(context, EVP_md5(), nullptr) != 1) {
      fail("EVP_DigestInit_ex failed");
    }
  }
  void add(const char* text) {
    if (failed) return;
    add(reinterpret_cast<const uint8_t*>(text), std::strlen(text));
  }
  void add(const uint8_t* bytes, const size_t count) {
    if (failed) return;
    auto& fixture = documentIdFixture::state;
    fixture.hashFeedSizes.push_back(count);
    // A baseline negative HalFile::read result wraps to SIZE_MAX. Record that
    // unsafe request without actually reading outside its 1024-byte buffer.
    if (count > 1024) {
      fixture.oversizedHashFeed = true;
      return;
    }
    if (injectedFailure(documentIdFixture::Md5Operation::Update) || EVP_DigestUpdate(context, bytes, count) != 1) {
      fail("EVP_DigestUpdate failed");
    }
  }
  void calculate() {
    if (failed) return;
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (injectedFailure(documentIdFixture::Md5Operation::Finalize) ||
        EVP_DigestFinal_ex(context, digest, &length) != 1 || length != 16) {
      fail("EVP_DigestFinal_ex failed");
      return;
    }
    static constexpr char HEX[] = "0123456789abcdef";
    value.clear();
    for (unsigned int i = 0; i < length; ++i) {
      value.push_back(HEX[digest[i] >> 4]);
      value.push_back(HEX[digest[i] & 15]);
    }
  }
  const std::string& toString() const { return value; }

 private:
  static bool injectedFailure(const documentIdFixture::Md5Operation operation) {
    auto& fixture = documentIdFixture::state;
    ++fixture.md5Calls[static_cast<size_t>(operation)];
    return fixture.failedMd5Operation == operation;
  }

  void fail(const char* reason) {
    if (failed) return;
    failed = true;
    value.clear();
    LOG_ERR("MD5Test", "%s", reason);
    ADD_FAILURE() << "MD5Builder adapter: " << reason;
  }

  EVP_MD_CTX* context = nullptr;
  bool failed = false;
  std::string value;
};
