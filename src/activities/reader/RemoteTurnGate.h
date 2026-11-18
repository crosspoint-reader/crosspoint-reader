#pragma once

// Whether a Bluetooth remote's page or chapter turn may reach the book in front, as a device key
// would: never while the page is still on its way to the panel, and never while a toolbar, panel
// or end-of-book menu over the page owns the device keys.
constexpr bool remoteTurnAccepted(const bool pageShown, const bool rendering, const bool inputOverPage) {
  return pageShown && !rendering && !inputOverPage;
}
