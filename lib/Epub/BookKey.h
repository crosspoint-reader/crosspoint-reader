#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Per-book content keys, stored next to the book as "<book>.key" and wrapped
// (AES-256-GCM) with a key derived from this device's secret: a copied card
// does not open on another reader, and the authenticated header keeps the
// expiry from being edited. The plugin that fulfils a book derives its key
// (whatever the scheme) and hands it over through POST /api/book-key; the
// reader then needs no scheme knowledge.
namespace bookkey {

constexpr size_t KEY_LEN = 16;  // AES-128

// expiresAt: epoch seconds, 0 = no expiry.
bool write(const std::string& bookPath, const uint8_t (&key)[KEY_LEN], int64_t expiresAt);

// False when the file is missing, not wrapped by this device, or tampered.
bool read(const std::string& bookPath, uint8_t (&key)[KEY_LEN], int64_t* expiresAt);

}  // namespace bookkey
