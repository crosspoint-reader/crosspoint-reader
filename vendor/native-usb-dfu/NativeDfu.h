#pragma once
namespace native_dfu {
bool begin();
bool connected();
void disconnect();
void reconnectForStorage();
}  // namespace native_dfu
