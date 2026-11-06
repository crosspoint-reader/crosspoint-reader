#pragma once

#include <cstdint>
#include <string>

// Device identity for the LCP service: an X25519 keypair generated on first
// use and enrolled once with /enroll, gated by the release key CI injects
// into official builds. /unlock responses are wrapped to the enrolled public
// key, so they are useless to any other party. Enrollment is identification,
// not attestation; the private key is stored obfuscated with the device key.
namespace lcpdevice {

// Loads the stored identity or generates + enrolls one. False on failure;
// notEnrollable=true means this build carries no release key (self-compiled)
// and enrollment can never succeed.
bool ensureEnrolled(std::string& deviceId, bool& notEnrollable);

// Drops the stored identity (server answered 401: revoked or registry reset);
// the next ensureEnrolled() enrolls fresh.
void forget();

// Unwraps a /unlock content_key object (base64 epk/iv/ct/tag: ephemeral
// X25519, HKDF-SHA256, AES-256-GCM) with the stored private key.
bool unwrapContentKey(const std::string& epkB64, const std::string& ivB64, const std::string& ctB64,
                      const std::string& tagB64, uint8_t out[32]);

}  // namespace lcpdevice
