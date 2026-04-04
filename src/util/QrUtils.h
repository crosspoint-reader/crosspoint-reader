#pragma once

#include <GfxRenderer.h>

#include <string>

#include "components/themes/BaseTheme.h"

namespace QrUtils {

// Maximum QR capacity in bytes (Version 40, ECC_LOW, byte mode).
static constexpr size_t MAX_QR_CAPACITY = 2953;

// Renders a QR code with the given text payload within the specified bounding box.
// Returns true if the payload was truncated to fit the QR capacity.
bool drawQrCode(const GfxRenderer& renderer, const Rect& bounds, const std::string& textPayload);

}  // namespace QrUtils
