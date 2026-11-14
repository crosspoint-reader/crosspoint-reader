#include "ExternalServices.h"

#include <Logging.h>
#include <Network.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <strings.h>

#include <cctype>
#include <cstring>

#include "CrossPointSettings.h"

namespace {

bool isZero(const IPAddress& ip) { return ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0; }

bool sameSubnet(const IPAddress& address, const IPAddress& local, const IPAddress& mask) {
  if (isZero(address) || isZero(local) || isZero(mask)) return false;
  for (size_t i = 0; i < 4; ++i) {
    if ((address[i] & mask[i]) != (local[i] & mask[i])) return false;
  }
  return true;
}

bool isOnLink(const IPAddress& address) {
  // Loopback and IPv4 link-local never leave the device/link.
  if (address[0] == 127 || (address[0] == 169 && address[1] == 254)) return true;
  if (WiFi.status() == WL_CONNECTED && sameSubnet(address, WiFi.localIP(), WiFi.subnetMask())) return true;

  const wifi_mode_t mode = WiFi.getMode();
  if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) {
    static const IPAddress AP_MASK(255, 255, 255, 0);
    if (sameSubnet(address, WiFi.softAPIP(), AP_MASK)) return true;
  }
  return false;
}

bool hasSuffixIgnoreCase(const char* value, const char* suffix) {
  const size_t valueLength = strlen(value);
  const size_t suffixLength = strlen(suffix);
  return valueLength >= suffixLength && strcasecmp(value + valueLength - suffixLength, suffix) == 0;
}

bool resolve(const char* host, IPAddress& address) {
  if (address.fromString(host)) return true;
  if (Network.hostByName(host, address) == 1) return true;
  LOG_INF("NETPOL", "Destination lookup failed: %s", host);
  return false;
}

}  // namespace

namespace externalservices {

bool enabled() { return SETTINGS.externalServicesEnabled != 0; }

void settingChanged() {
  if (!enabled() && esp_sntp_enabled()) esp_sntp_stop();
}

bool resolveDestination(const char* host, IPAddress& address) {
  if (!host || host[0] == '\0') return false;
  if (enabled()) return resolve(host, address);

  if (address.fromString(host)) {
    const bool allowed = isOnLink(address);
    if (!allowed) LOG_INF("NETPOL", "Blocked off-link destination: %s", host);
    return allowed;
  }

  // localhost never needs DNS. Normalize .local before mDNS lookup: lwIP's
  // special-name routing is case-sensitive even though DNS names are not.
  if (strcasecmp(host, "localhost") == 0) return address.fromString("127.0.0.1");

  const bool mdnsName = hasSuffixIgnoreCase(host, ".local");
  if (!mdnsName) {
    LOG_INF("NETPOL", "Blocked non-mDNS hostname while external services are disabled: %s", host);
    return false;
  }

  const char* lookupHost = host;
  char normalizedHost[254];
  if (mdnsName) {
    const size_t length = strlen(host);
    if (length >= sizeof(normalizedHost)) return false;
    for (size_t i = 0; i <= length; ++i) {
      normalizedHost[i] = static_cast<char>(tolower(static_cast<unsigned char>(host[i])));
    }
    lookupHost = normalizedHost;
  }
  if (!resolve(lookupHost, address)) return false;

  const bool allowed = isOnLink(address);
  if (!allowed) LOG_INF("NETPOL", "Blocked hostname resolving off-link: %s", host);
  return allowed;
}

}  // namespace externalservices
