#pragma once

/**
 * Shared network constants used by both the File Transfer web server
 * (CrossPointWebServerActivity) and the Keyboard remote input server
 * (KeyboardWebInputServer).
 */
namespace NetworkConstants {

constexpr const char* AP_SSID = "CrossPoint-Reader";
constexpr const char* AP_PASSWORD = nullptr;  // Open network for ease of use
constexpr const char* AP_HOSTNAME = "crosspoint";
constexpr uint8_t AP_CHANNEL = 1;
constexpr uint8_t AP_MAX_CONNECTIONS = 4;
constexpr uint16_t HTTP_PORT = 80;
constexpr uint16_t DNS_PORT = 53;

}  // namespace NetworkConstants
