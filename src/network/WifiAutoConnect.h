#pragma once

#include <string>

enum class WifiAutoConnectResult { Connected, NoCredentials, Failed, Timeout };

// Connect to the last saved Wi-Fi network from WifiCredentialStore.
// ssidOut receives the SSID attempted on success or failure (empty on NoCredentials).
WifiAutoConnectResult connectLastSaved(std::string& ssidOut);
