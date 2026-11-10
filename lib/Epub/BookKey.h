#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Per-book content keys, stored next to the book as "<book>.key" and wrapped
// (AES-256-GCM) with a key derived from this device's secret: a copied card
// does not open on another reader, and the authenticated header keeps the
// expiry from being edited. Whoever fulfils a book derives its key (ADEPT
// plugin, LCP passphrase unlock — whatever the scheme) and stores it here;
// the reader then needs no scheme knowledge.
namespace bookkey {

// AES-128 (ADEPT) or AES-256 (LCP) content keys.
constexpr size_t KEY_LEN = 16;
constexpr size_t MAX_KEY_LEN = 32;

// expiresAt: epoch seconds, 0 = no expiry. keyLen: 16 or 32.
bool write(const std::string& bookPath, const uint8_t* key, size_t keyLen, int64_t expiresAt);

// Returns the key length (16 or 32), or 0 when the file is missing, not
// wrapped by this device, or tampered.
size_t read(const std::string& bookPath, uint8_t (&key)[MAX_KEY_LEN], int64_t* expiresAt);

}  // namespace bookkey
