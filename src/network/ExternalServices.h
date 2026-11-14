#pragma once

class IPAddress;

namespace externalservices {

// Persistent opt-in used by non-HTTP public services such as NTP.
bool enabled();

// Applies immediate runtime consequences of a persisted setting transition.
void settingChanged();

// SecureNet resolver policy. Returns the exact address the transport may use,
// so DNS validation and TCP connection cannot diverge.
bool resolveDestination(const char* host, IPAddress& address);

}  // namespace externalservices
